﻿/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Pierre-Luc Bacon <pierre-luc.bacon@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "sipaccount.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compiler_intrinsics.h"

#include "sdp.h"
#include "sipvoiplink.h"
#include "sipcall.h"
#include "sip_utils.h"

#include "call_factory.h"

#include "sippresence.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <yaml-cpp/yaml.h>
#pragma GCC diagnostic pop

#include "account_schema.h"
#include "config/yamlparser.h"
#include "logger.h"
#include "manager.h"
#include "client/ring_signal.h"
#include "jami/account_const.h"

#ifdef ENABLE_VIDEO
#include "libav_utils.h"
#endif

#include "system_codec_container.h"

#include "upnp/upnp_control.h"
#include "ip_utils.h"
#include "string_utils.h"

#include "im/instant_messaging.h"

#include <opendht/crypto.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <ctime>
#include <charconv>

#ifdef _WIN32
#include <lmcons.h>
#else
#include <pwd.h>
#endif

namespace jami {

using yaml_utils::parseValue;
using yaml_utils::parseValueOptional;
using yaml_utils::parseVectorMap;
using sip_utils::CONST_PJ_STR;

static constexpr int MIN_REGISTRATION_TIME = 60;                  // seconds
static constexpr unsigned DEFAULT_REGISTRATION_EXPIRE = 3600;     // seconds
static constexpr unsigned REGISTRATION_FIRST_RETRY_INTERVAL = 60; // seconds
static constexpr unsigned REGISTRATION_RETRY_INTERVAL = 300;      // seconds
static const char* const VALID_TLS_PROTOS[] = {"Default", "TLSv1.2", "TLSv1.1", "TLSv1"};
constexpr const char* const SIPAccount::ACCOUNT_TYPE;

struct ctx
{
    ctx(pjsip_auth_clt_sess* auth)
        : auth_sess(auth, &pjsip_auth_clt_deinit)
    {}
    std::weak_ptr<SIPAccount> acc;
    std::string to;
    uint64_t id;
    std::unique_ptr<pjsip_auth_clt_sess, decltype(&pjsip_auth_clt_deinit)> auth_sess;
};

static void
registration_cb(pjsip_regc_cbparam* param)
{
    if (!param) {
        JAMI_ERR("registration callback parameter is null");
        return;
    }

    auto account = static_cast<SIPAccount*>(param->token);
    if (!account) {
        JAMI_ERR("account doesn't exist in registration callback");
        return;
    }

    account->onRegister(param);
}

SIPAccount::SIPAccount(const std::string& accountID, bool presenceEnabled)
    : SIPAccountBase(accountID)
    , auto_rereg_()
    , credentials_()
    , regc_(nullptr)
    , bRegister_(false)
    , registrationExpire_(DEFAULT_REGISTRATION_EXPIRE)
    , serviceRoute_()
    , cred_()
    , tlsSetting_()
    , ciphers_(100)
    , tlsMethod_("TLSv1")
    , tlsCiphers_()
    , tlsServerName_("")
    , tlsVerifyServer_(false)
    , tlsVerifyClient_(true)
    , tlsRequireClientCertificate_(true)
    , tlsNegotiationTimeoutSec_("2")
    , registrationStateDetailed_()
    , registrationRefreshEnabled_(true)
    , receivedParameter_("")
    , rPort_(-1)
    , via_addr_()
    , allowIPAutoRewrite_(true)
    , via_tp_(nullptr)
    , presence_(presenceEnabled ? new SIPPresence(this) : nullptr)
{
    via_addr_.host.ptr = 0;
    via_addr_.host.slen = 0;
    via_addr_.port = 0;
    setActiveCodecs({});
}

SIPAccount::~SIPAccount() noexcept
{
    // ensure that no registration callbacks survive past this point
    destroyRegistrationInfo();
    setTransport();

    delete presence_;
}

std::shared_ptr<SIPCall>
SIPAccount::newIncomingCall(const std::string& from UNUSED,
                            const std::vector<DRing::MediaMap>& mediaList,
                            const std::shared_ptr<SipTransport>& transport)
{
    auto call = Manager::instance().callFactory.newSipCall(shared(),
                                                           Call::CallType::INCOMING,
                                                           mediaList);
    call->setSipTransport(transport, getContactHeader());
    return call;
}

std::shared_ptr<Call>
SIPAccount::newOutgoingCall(std::string_view toUrl, const std::vector<DRing::MediaMap>& mediaList)
{
    std::string to;
    int family;

    JAMI_DBG() << *this << "Calling SIP peer " << toUrl;

    auto& manager = Manager::instance();
    std::shared_ptr<SIPCall> call;

    // SIP allows sending empty invites.
    if (not mediaList.empty() or isEmptyOffersEnabled()) {
        call = manager.callFactory.newSipCall(shared(), Call::CallType::OUTGOING, mediaList);
    } else {
        JAMI_WARN("Media list is empty, setting a default list");
        call = manager.callFactory.newSipCall(shared(),
                                              Call::CallType::OUTGOING,
                                              MediaAttribute::mediaAttributesToMediaMaps(
                                                  createDefaultMediaList(isVideoEnabled())));
    }

    if (not call)
        throw std::runtime_error("Failed to create the call");

    if (isIP2IP()) {
        bool ipv6 = IpAddr::isIpv6(toUrl);
        to = ipv6 ? IpAddr(toUrl).toString(false, true) : toUrl;
        family = ipv6 ? pj_AF_INET6() : pj_AF_INET();

        // TODO: resolve remote host using SIPVoIPLink::resolveSrvName
        std::shared_ptr<SipTransport> t
            = isTlsEnabled()
                  ? link_.sipTransportBroker->getTlsTransport(tlsListener_,
                                                              IpAddr(sip_utils::getHostFromUri(to)))
                  : transport_;
        setTransport(t);
        call->setSipTransport(t, getContactHeader());

        JAMI_DBG("New %s IP to IP call to %s", ipv6 ? "IPv6" : "IPv4", to.c_str());
    } else {
        to = toUrl;
        call->setSipTransport(transport_, getContactHeader());
        // Use the same address family as the SIP transport
        family = pjsip_transport_type_get_af(getTransportType());

        JAMI_DBG("UserAgent: New registered account call to %.*s", (int) toUrl.size(), toUrl.data());
    }

    auto toUri = getToUri(to);

    // Do not init ICE yet if the media list is empty. This may occur
    // if we are sending an invite with no SDP offer.
    if (call->isIceEnabled() and not mediaList.empty()) {
        if (call->createIceMediaTransport(false)) {
            call->initIceMediaTransport(true);
        }
    }

    call->setPeerNumber(toUri);
    call->setPeerUri(toUri);

    const auto localAddress = ip_utils::getInterfaceAddr(getLocalInterface(), family);

    IpAddr addrSdp;
    if (getUPnPActive()) {
        /* use UPnP addr, or published addr if its set */
        addrSdp = getPublishedSameasLocal() ? getUPnPIpAddress() : getPublishedIpAddress();
    } else {
        addrSdp = isStunEnabled() or (not getPublishedSameasLocal()) ? getPublishedIpAddress()
                                                                     : localAddress;
    }

    /* fallback on local address */
    if (not addrSdp)
        addrSdp = localAddress;

    // Building the local SDP offer
    auto& sdp = call->getSDP();

    if (getPublishedSameasLocal())
        sdp.setPublishedIP(addrSdp);
    else
        sdp.setPublishedIP(getPublishedAddress());

    // TODO. We should not dot his here. Move it to SIPCall.
    const bool created = sdp.createOffer(
        MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled()));

    if (created) {
        std::weak_ptr<SIPCall> weak_call = call;
        manager.scheduler().run([this, weak_call] {
            if (auto call = weak_call.lock()) {
                if (not SIPStartCall(call)) {
                    JAMI_ERR("Could not send outgoing INVITE request for new call");
                    call->onFailure();
                }
            }
            return false;
        });
    } else {
        throw VoipLinkException("Could not send outgoing INVITE request for new call");
    }

    return call;
}

void
SIPAccount::onTransportStateChanged(pjsip_transport_state state,
                                    const pjsip_transport_state_info* info)
{
    pj_status_t currentStatus = transportStatus_;
    JAMI_DBG("Transport state changed to %s for account %s !",
             SipTransport::stateToStr(state),
             accountID_.c_str());
    if (!SipTransport::isAlive(state)) {
        if (info) {
            transportStatus_ = info->status;
            transportError_ = sip_utils::sip_strerror(info->status);
            JAMI_ERR("Transport disconnected: %s", transportError_.c_str());
        } else {
            // This is already the generic error used by pjsip.
            transportStatus_ = PJSIP_SC_SERVICE_UNAVAILABLE;
            transportError_ = "";
        }
        setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_TSX_TRANSPORT_ERROR);
        setTransport();
    } else {
        // The status can be '0', this is the same as OK
        transportStatus_ = info && info->status ? info->status : PJSIP_SC_OK;
        transportError_ = "";
    }

    // Notify the client of the new transport state
    if (currentStatus != transportStatus_)
        emitSignal<DRing::ConfigurationSignal::VolatileDetailsChanged>(accountID_,
                                                                       getVolatileAccountDetails());
}

void
SIPAccount::setTransport(const std::shared_ptr<SipTransport>& t)
{
    if (t == transport_)
        return;
    if (transport_) {
        JAMI_DBG("Removing old transport [%p] from account", transport_.get());
        if (regc_)
            pjsip_regc_release_transport(regc_);
        transport_->removeStateListener(reinterpret_cast<uintptr_t>(this));
    }

    transport_ = t;
    JAMI_DBG("Set new transport [%p]", transport_.get());

    if (transport_) {
        transport_->addStateListener(reinterpret_cast<uintptr_t>(this),
                                     std::bind(&SIPAccount::onTransportStateChanged,
                                               this,
                                               std::placeholders::_1,
                                               std::placeholders::_2));
        // Update contact address and header
        if (not initContactAddress()) {
            JAMI_DBG("Can not register: invalid address");
            return;
        }
        updateContactHeader();
    }
}

pjsip_tpselector
SIPAccount::getTransportSelector()
{
    if (!transport_)
        return SIPVoIPLink::getTransportSelector(nullptr);
    return SIPVoIPLink::getTransportSelector(transport_->get());
}

bool
SIPAccount::SIPStartCall(std::shared_ptr<SIPCall>& call)
{
    // Add Ice headers to local SDP if ice transport exist
    call->addLocalIceAttributes();

    const std::string& toUri(call->getPeerNumber()); // expecting a fully well formed sip uri
    pj_str_t pjTo = sip_utils::CONST_PJ_STR(toUri);

    // Create the from header
    std::string from(getFromUri());
    pj_str_t pjFrom = sip_utils::CONST_PJ_STR(from);

    auto transport = call->getTransport();
    if (!transport) {
        JAMI_ERR("Unable to start call without transport");
        return false;
    }

    std::string contact = getContactHeader();
    JAMI_DBG("contact header: %s / %s -> %s", contact.c_str(), from.c_str(), toUri.c_str());

    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);
    auto local_sdp = isEmptyOffersEnabled() ? nullptr : call->getSDP().getLocalSdpSession();

    pjsip_dialog* dialog {nullptr};
    pjsip_inv_session* inv {nullptr};
    if (!CreateClientDialogAndInvite(&pjFrom, &pjContact, &pjTo, nullptr, local_sdp, &dialog, &inv))
        return false;

    inv->mod_data[link_.getModId()] = call.get();
    call->setInviteSession(inv);

    updateDialogViaSentBy(dialog);

    if (hasServiceRoute())
        pjsip_dlg_set_route_set(dialog,
                                sip_utils::createRouteSet(getServiceRoute(),
                                                          call->inviteSession_->pool));

    if (hasCredentials()
        and pjsip_auth_clt_set_credentials(&dialog->auth_sess, getCredentialCount(), getCredInfo())
                != PJ_SUCCESS) {
        JAMI_ERR("Could not initialize credentials for invite session authentication");
        return false;
    }

    pjsip_tx_data* tdata;

    if (pjsip_inv_invite(call->inviteSession_.get(), &tdata) != PJ_SUCCESS) {
        JAMI_ERR("Could not initialize invite messager for this call");
        return false;
    }

    const pjsip_tpselector tp_sel = link_.getTransportSelector(transport->get());
    if (pjsip_dlg_set_transport(dialog, &tp_sel) != PJ_SUCCESS) {
        JAMI_ERR("Unable to associate transport for invite session dialog");
        return false;
    }

    // Add user-agent header
    sip_utils::addUserAgentHeader(getUserAgentName(), tdata);

    if (pjsip_inv_send_msg(call->inviteSession_.get(), tdata) != PJ_SUCCESS) {
        JAMI_ERR("Unable to send invite message for this call");
        return false;
    }

    call->setState(Call::CallState::ACTIVE, Call::ConnectionState::PROGRESSING);

    return true;
}

void
SIPAccount::serialize(YAML::Emitter& out) const
{
    std::lock_guard<std::recursive_mutex> lock(configurationMutex_);

    out << YAML::BeginMap;
    SIPAccountBase::serialize(out);

    out << YAML::Key << Conf::BIND_ADDRESS_KEY << YAML::Value << bindAddress_;
    out << YAML::Key << Conf::PORT_KEY << YAML::Value << localPort_;

    out << YAML::Key << USERNAME_KEY << YAML::Value << username_;

    // each credential is a map, and we can have multiple credentials
    out << YAML::Key << Conf::CRED_KEY << YAML::Value << getCredentials();

    out << YAML::Key << Conf::KEEP_ALIVE_ENABLED << YAML::Value << registrationRefreshEnabled_;

    out << YAML::Key << PRESENCE_MODULE_ENABLED_KEY << YAML::Value
        << (presence_ and presence_->isEnabled());
    out << YAML::Key << Conf::PRESENCE_PUBLISH_SUPPORTED_KEY << YAML::Value
        << (presence_ and presence_->isSupported(PRESENCE_FUNCTION_PUBLISH));
    out << YAML::Key << Conf::PRESENCE_SUBSCRIBE_SUPPORTED_KEY << YAML::Value
        << (presence_ and presence_->isSupported(PRESENCE_FUNCTION_SUBSCRIBE));

    out << YAML::Key << Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE << YAML::Value
        << registrationExpire_;
    out << YAML::Key << Conf::SERVICE_ROUTE_KEY << YAML::Value << serviceRoute_;
    out << YAML::Key << Conf::ALLOW_IP_AUTO_REWRITE << YAML::Value << allowIPAutoRewrite_;

    // tls submap
    out << YAML::Key << Conf::TLS_KEY << YAML::Value << YAML::BeginMap;
    SIPAccountBase::serializeTls(out);
    out << YAML::Key << Conf::TLS_ENABLE_KEY << YAML::Value << tlsEnable_;
    out << YAML::Key << Conf::TLS_PORT_KEY << YAML::Value << tlsListenerPort_;
    out << YAML::Key << Conf::VERIFY_CLIENT_KEY << YAML::Value << tlsVerifyClient_;
    out << YAML::Key << Conf::VERIFY_SERVER_KEY << YAML::Value << tlsVerifyServer_;
    out << YAML::Key << Conf::REQUIRE_CERTIF_KEY << YAML::Value << tlsRequireClientCertificate_;
    out << YAML::Key << Conf::TIMEOUT_KEY << YAML::Value << tlsNegotiationTimeoutSec_;
    out << YAML::Key << Conf::CIPHERS_KEY << YAML::Value << tlsCiphers_;
    out << YAML::Key << Conf::METHOD_KEY << YAML::Value << tlsMethod_;
    out << YAML::Key << Conf::SERVER_KEY << YAML::Value << tlsServerName_;
    out << YAML::EndMap;

    // srtp submap
    out << YAML::Key << Conf::SRTP_KEY << YAML::Value << YAML::BeginMap;
    out << YAML::Key << Conf::KEY_EXCHANGE_KEY << YAML::Value
        << sip_utils::getKeyExchangeName(srtpKeyExchange_);
    out << YAML::Key << Conf::RTP_FALLBACK_KEY << YAML::Value << srtpFallback_;
    out << YAML::EndMap;

    out << YAML::EndMap;
}

void
SIPAccount::usePublishedAddressPortInVIA()
{
    publishedIpStr_ = getPublishedIpAddress().toString();
    via_addr_.host.ptr = (char*) publishedIpStr_.c_str();
    via_addr_.host.slen = publishedIpStr_.size();
    via_addr_.port = publishedPort_;
}

void
SIPAccount::useUPnPAddressPortInVIA()
{
    upnpIpAddr_ = getUPnPIpAddress().toString();
    via_addr_.host.ptr = (char*) upnpIpAddr_.c_str();
    via_addr_.host.slen = upnpIpAddr_.size();
    via_addr_.port = publishedPortUsed_;
}

template<typename T>
static void
validate(std::string& member, const std::string& param, const T& valid)
{
    const auto begin = std::begin(valid);
    const auto end = std::end(valid);
    if (find(begin, end, param) != end)
        member = param;
    else
        JAMI_ERR("Invalid parameter \"%s\"", param.c_str());
}

void
SIPAccount::unserialize(const YAML::Node& node)
{
    std::lock_guard<std::recursive_mutex> lock(configurationMutex_);

    SIPAccountBase::unserialize(node);
    parseValue(node, USERNAME_KEY, username_);

    parseValue(node, Conf::BIND_ADDRESS_KEY, bindAddress_);

    int port = sip_utils::DEFAULT_SIP_PORT;
    parseValue(node, Conf::PORT_KEY, port);
    localPort_ = port;

    if (not isIP2IP()) {
        unsigned expire = 0;
        if (not parseValueOptional(node, Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE, expire)) {
            // Proably using an older config file.
            parseValueOptional(node, Preferences::REGISTRATION_EXPIRE_KEY, expire);
        }
        setRegistrationExpire(expire);

        parseValue(node, Conf::KEEP_ALIVE_ENABLED, registrationRefreshEnabled_);
        parseValue(node, Conf::SERVICE_ROUTE_KEY, serviceRoute_);
        parseValueOptional(node, Conf::ALLOW_IP_AUTO_REWRITE, allowIPAutoRewrite_);

        const auto& credsNode = node[Conf::CRED_KEY];
        setCredentials(parseVectorMap(credsNode,
                                      {Conf::CONFIG_ACCOUNT_REALM,
                                       Conf::CONFIG_ACCOUNT_USERNAME,
                                       Conf::CONFIG_ACCOUNT_PASSWORD}));
    }

    bool presEnabled = false;
    parseValue(node, PRESENCE_MODULE_ENABLED_KEY, presEnabled);
    enablePresence(presEnabled);
    bool publishSupported = false;
    parseValue(node, Conf::PRESENCE_PUBLISH_SUPPORTED_KEY, publishSupported);
    bool subscribeSupported = false;
    parseValue(node, Conf::PRESENCE_SUBSCRIBE_SUPPORTED_KEY, subscribeSupported);
    if (presence_) {
        presence_->support(PRESENCE_FUNCTION_PUBLISH, publishSupported);
        presence_->support(PRESENCE_FUNCTION_SUBSCRIBE, subscribeSupported);
    }

    // Init stun server name with default server name
    stunServerName_ = pj_str((char*) stunServer_.data());

    const auto& credsNode = node[Conf::CRED_KEY];
    setCredentials(parseVectorMap(credsNode,
                                  {Conf::CONFIG_ACCOUNT_REALM,
                                   Conf::CONFIG_ACCOUNT_USERNAME,
                                   Conf::CONFIG_ACCOUNT_PASSWORD}));

    // get tls submap
    const auto& tlsMap = node[Conf::TLS_KEY];
    parseValue(tlsMap, Conf::CERTIFICATE_KEY, tlsCertificateFile_);
    parseValue(tlsMap, Conf::CALIST_KEY, tlsCaListFile_);
    parseValue(tlsMap, Conf::TLS_PASSWORD_KEY, tlsPassword_);
    parseValue(tlsMap, Conf::PRIVATE_KEY_KEY, tlsPrivateKeyFile_);
    parseValue(tlsMap, Conf::TLS_ENABLE_KEY, tlsEnable_);
    parseValue(tlsMap, Conf::TLS_PORT_KEY, tlsListenerPort_);
    parseValue(tlsMap, Conf::CIPHERS_KEY, tlsCiphers_);

    std::string tmpMethod(tlsMethod_);
    parseValue(tlsMap, Conf::METHOD_KEY, tmpMethod);
    validate(tlsMethod_, tmpMethod, VALID_TLS_PROTOS);

    parseValue(tlsMap, Conf::SERVER_KEY, tlsServerName_);
    parseValue(tlsMap, Conf::REQUIRE_CERTIF_KEY, tlsRequireClientCertificate_);
    parseValue(tlsMap, Conf::VERIFY_CLIENT_KEY, tlsVerifyClient_);
    parseValue(tlsMap, Conf::VERIFY_SERVER_KEY, tlsVerifyServer_);
    // FIXME
    parseValue(tlsMap, Conf::TIMEOUT_KEY, tlsNegotiationTimeoutSec_);

    // get srtp submap
    const auto& srtpMap = node[Conf::SRTP_KEY];
    std::string tmpKey;
    parseValue(srtpMap, Conf::KEY_EXCHANGE_KEY, tmpKey);
    srtpKeyExchange_ = sip_utils::getKeyExchangeProtocol(tmpKey.c_str());
    parseValue(srtpMap, Conf::RTP_FALLBACK_KEY, srtpFallback_);
}

void
SIPAccount::setAccountDetails(const std::map<std::string, std::string>& details)
{
    std::lock_guard<std::recursive_mutex> lock(configurationMutex_);

    SIPAccountBase::setAccountDetails(details);
    parseString(details, Conf::CONFIG_ACCOUNT_USERNAME, username_);

    parseInt(details, Conf::CONFIG_LOCAL_PORT, localPort_);

    // TLS
    parseString(details, Conf::CONFIG_TLS_CA_LIST_FILE, tlsCaListFile_);
    parseString(details, Conf::CONFIG_TLS_CERTIFICATE_FILE, tlsCertificateFile_);
    parseString(details, Conf::CONFIG_TLS_PRIVATE_KEY_FILE, tlsPrivateKeyFile_);
    parseString(details, Conf::CONFIG_TLS_PASSWORD, tlsPassword_);

    // SIP specific account settings
    parseString(details, Conf::CONFIG_BIND_ADDRESS, bindAddress_);
    parseString(details, Conf::CONFIG_ACCOUNT_ROUTESET, serviceRoute_);
    parseBool(details, Conf::CONFIG_ACCOUNT_IP_AUTO_REWRITE, allowIPAutoRewrite_);

    unsigned expire = 0;
    parseInt(details, Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE, expire);
    setRegistrationExpire(expire);

    parseBool(details, Conf::CONFIG_KEEP_ALIVE_ENABLED, registrationRefreshEnabled_);
    bool presenceEnabled = false;
    parseBool(details, Conf::CONFIG_PRESENCE_ENABLED, presenceEnabled);
    enablePresence(presenceEnabled);

    // TLS settings
    parseBool(details, Conf::CONFIG_TLS_ENABLE, tlsEnable_);
    parseInt(details, Conf::CONFIG_TLS_LISTENER_PORT, tlsListenerPort_);
    auto iter = details.find(Conf::CONFIG_TLS_METHOD);
    if (iter != details.end())
        validate(tlsMethod_, iter->second, VALID_TLS_PROTOS);
    parseString(details, Conf::CONFIG_TLS_CIPHERS, tlsCiphers_);
    parseString(details, Conf::CONFIG_TLS_SERVER_NAME, tlsServerName_);
    parseBool(details, Conf::CONFIG_TLS_VERIFY_SERVER, tlsVerifyServer_);
    parseBool(details, Conf::CONFIG_TLS_VERIFY_CLIENT, tlsVerifyClient_);
    parseBool(details, Conf::CONFIG_TLS_REQUIRE_CLIENT_CERTIFICATE, tlsRequireClientCertificate_);
    parseString(details, Conf::CONFIG_TLS_NEGOTIATION_TIMEOUT_SEC, tlsNegotiationTimeoutSec_);
    parseBool(details, Conf::CONFIG_TLS_VERIFY_SERVER, tlsVerifyServer_);
    parseBool(details, Conf::CONFIG_TLS_VERIFY_CLIENT, tlsVerifyClient_);
    parseBool(details, Conf::CONFIG_TLS_REQUIRE_CLIENT_CERTIFICATE, tlsRequireClientCertificate_);
    parseString(details, Conf::CONFIG_TLS_NEGOTIATION_TIMEOUT_SEC, tlsNegotiationTimeoutSec_);

    // srtp settings
    parseBool(details, Conf::CONFIG_SRTP_RTP_FALLBACK, srtpFallback_);
    iter = details.find(Conf::CONFIG_SRTP_KEY_EXCHANGE);
    if (iter != details.end())
        srtpKeyExchange_ = sip_utils::getKeyExchangeProtocol(iter->second.c_str());

    if (credentials_.empty()) { // credentials not set, construct 1 entry
        JAMI_WARN("No credentials set, inferring them...");
        std::vector<std::map<std::string, std::string>> v;
        std::map<std::string, std::string> map;
        map[Conf::CONFIG_ACCOUNT_USERNAME] = username_;
        parseString(details, Conf::CONFIG_ACCOUNT_PASSWORD, map[Conf::CONFIG_ACCOUNT_PASSWORD]);
        map[Conf::CONFIG_ACCOUNT_REALM] = "*";
        v.push_back(map);
        setCredentials(v);
    }
    if (isIP2IP()) {
        localPort_ = (localPort_ == sip_utils::DEFAULT_AUTO_SELECT_PORT)
                         ? sip_utils::DEFAULT_SIP_PORT
                         : localPort_;
        tlsListenerPort_ = (tlsListenerPort_ == sip_utils::DEFAULT_AUTO_SELECT_PORT)
                               ? sip_utils::DEFAULT_SIP_TLS_PORT
                               : tlsListenerPort_;
    }
}

std::map<std::string, std::string>
SIPAccount::getAccountDetails() const
{
    std::lock_guard<std::recursive_mutex> lock(configurationMutex_);

    auto a = SIPAccountBase::getAccountDetails();

    std::string password {};
    if (hasCredentials()) {
        for (const auto& cred : credentials_)
            if (cred.username == username_) {
                password = cred.password;
                break;
            }
    }
    a.emplace(Conf::CONFIG_ACCOUNT_PASSWORD, std::move(password));

    a.emplace(Conf::CONFIG_BIND_ADDRESS, bindAddress_);
    a.emplace(Conf::CONFIG_LOCAL_PORT, std::to_string(localPort_));
    a.emplace(Conf::CONFIG_ACCOUNT_ROUTESET, serviceRoute_);
    a.emplace(Conf::CONFIG_ACCOUNT_IP_AUTO_REWRITE, allowIPAutoRewrite_ ? TRUE_STR : FALSE_STR);
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE, std::to_string(registrationExpire_));
    a.emplace(Conf::CONFIG_KEEP_ALIVE_ENABLED, registrationRefreshEnabled_ ? TRUE_STR : FALSE_STR);

    a.emplace(Conf::CONFIG_PRESENCE_ENABLED,
              presence_ and presence_->isEnabled() ? TRUE_STR : FALSE_STR);
    a.emplace(Conf::CONFIG_PRESENCE_PUBLISH_SUPPORTED,
              presence_ and presence_->isSupported(PRESENCE_FUNCTION_PUBLISH) ? TRUE_STR
                                                                              : FALSE_STR);
    a.emplace(Conf::CONFIG_PRESENCE_SUBSCRIBE_SUPPORTED,
              presence_ and presence_->isSupported(PRESENCE_FUNCTION_SUBSCRIBE) ? TRUE_STR
                                                                                : FALSE_STR);

    auto tlsSettings(getTlsSettings());
    a.insert(tlsSettings.begin(), tlsSettings.end());

    a.emplace(Conf::CONFIG_SRTP_KEY_EXCHANGE, sip_utils::getKeyExchangeName(srtpKeyExchange_));
    a.emplace(Conf::CONFIG_SRTP_ENABLE, isSrtpEnabled() ? TRUE_STR : FALSE_STR);
    a.emplace(Conf::CONFIG_SRTP_RTP_FALLBACK, srtpFallback_ ? TRUE_STR : FALSE_STR);

    return a;
}

std::map<std::string, std::string>
SIPAccount::getVolatileAccountDetails() const
{
    auto a = SIPAccountBase::getVolatileAccountDetails();
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_STATE_CODE,
              std::to_string(registrationStateDetailed_.first));
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_STATE_DESC, registrationStateDetailed_.second);
    a.emplace(DRing::Account::VolatileProperties::InstantMessaging::OFF_CALL, TRUE_STR);

    if (presence_) {
        a.emplace(Conf::CONFIG_PRESENCE_STATUS, presence_->isOnline() ? TRUE_STR : FALSE_STR);
        a.emplace(Conf::CONFIG_PRESENCE_NOTE, presence_->getNote());
    }

    if (transport_ and transport_->isSecure() and transport_->isConnected()) {
        const auto& tlsInfos = transport_->getTlsInfos();
        auto cipher = pj_ssl_cipher_name(tlsInfos.cipher);
        if (tlsInfos.cipher and not cipher)
            JAMI_WARN("Unknown cipher: %d", tlsInfos.cipher);
        a.emplace(DRing::TlsTransport::TLS_CIPHER, cipher ? cipher : "");
        a.emplace(DRing::TlsTransport::TLS_PEER_CERT, tlsInfos.peerCert->toString());
        auto ca = tlsInfos.peerCert->issuer;
        unsigned n = 0;
        while (ca) {
            std::ostringstream name_str;
            name_str << DRing::TlsTransport::TLS_PEER_CA_ << n++;
            a.emplace(name_str.str(), ca->toString());
            ca = ca->issuer;
        }
        a.emplace(DRing::TlsTransport::TLS_PEER_CA_NUM, std::to_string(n));
    }

    return a;
}

bool
SIPAccount::mapPortUPnP()
{
    upnp::Mapping map(upnp::PortType::UDP, publishedPort_, localPort_);
    map.setNotifyCallback([w = weak()](upnp::Mapping::sharedPtr_t mapRes) {
        if (auto accPtr = w.lock()) {
            auto oldPort = static_cast<in_port_t>(accPtr->publishedPortUsed_);
            bool success = mapRes->getState() == upnp::MappingState::OPEN
                           or mapRes->getState() == upnp::MappingState::IN_PROGRESS;
            auto newPort = success ? mapRes->getExternalPort() : accPtr->publishedPort_;
            if (not success and not accPtr->isRegistered()) {
                JAMI_WARN("[Account %s] Failed to open port %u: registering SIP account anyway",
                          accPtr->getAccountID().c_str(),
                          oldPort);
                accPtr->doRegister1_();
                return;
            }
            if ((oldPort != newPort)
                or (accPtr->getRegistrationState() != RegistrationState::REGISTERED)) {
                if (not accPtr->isRegistered())
                    JAMI_WARN("[Account %s] SIP port %u opened: registering SIP account",
                              accPtr->getAccountID().c_str(),
                              newPort);
                else
                    JAMI_WARN("[Account %s] SIP port changed to %u: re-registering SIP account",
                              accPtr->getAccountID().c_str(),
                              newPort);
                accPtr->publishedPortUsed_ = newPort;
            } else {
                accPtr->connectivityChanged();
            }

            accPtr->doRegister1_();
        }
    });

    auto mapRes = upnpCtrl_->reserveMapping(map);
    if (mapRes and mapRes->getState() == upnp::MappingState::OPEN) {
        return true;
    }

    return false;
}

void
SIPAccount::setPushNotificationToken(const std::string& pushDeviceToken)
{
    JAMI_WARN("[SIP Account %s] setPushNotificationToken: %s",
              getAccountID().c_str(),
              pushDeviceToken.c_str());

    if (deviceKey_ == pushDeviceToken)
        return;

    deviceKey_ = pushDeviceToken;

    if (enabled_)
        doUnregister([&](bool /* transport_free */) { doRegister(); });
}

void
SIPAccount::pushNotificationReceived(const std::string& from,
                                     const std::map<std::string, std::string>&)
{
    JAMI_WARN("[SIP Account %s] pushNotificationReceived: %s", getAccountID().c_str(), from.c_str());

    if (enabled_)
        doUnregister([&](bool /* transport_free */) { doRegister(); });
}

void
SIPAccount::doRegister()
{
    if (not isUsable()) {
        JAMI_WARN("Account must be enabled and active to register, ignoring");
        return;
    }

    JAMI_DBG("doRegister %s", hostname_.c_str());

    /* if UPnP is enabled, then wait for IGD to complete registration */
    if (upnpCtrl_) {
        JAMI_DBG("UPnP: waiting for IGD to register SIP account");
        setRegistrationState(RegistrationState::TRYING);
        if (not mapPortUPnP()) {
            JAMI_DBG("UPnP: UPNP request failed, try to register SIP account anyway");
            doRegister1_();
        }
    } else {
        doRegister1_();
    }
}

void
SIPAccount::doRegister1_()
{
    {
        std::lock_guard<std::recursive_mutex> lock(configurationMutex_);
        if (isIP2IP()) {
            doRegister2_();
            return;
        }
    }

    link_.resolveSrvName(hasServiceRoute() ? getServiceRoute() : hostname_,
                         tlsEnable_ ? PJSIP_TRANSPORT_TLS : PJSIP_TRANSPORT_UDP,
                         [w = weak()](std::vector<IpAddr> host_ips) {
                             if (auto acc = w.lock()) {
                                 std::lock_guard<std::recursive_mutex> lock(
                                     acc->configurationMutex_);
                                 if (host_ips.empty()) {
                                     JAMI_ERR("Can't resolve hostname for registration.");
                                     acc->setRegistrationState(RegistrationState::ERROR_GENERIC,
                                                               PJSIP_SC_NOT_FOUND);
                                     return;
                                 }
                                 acc->hostIp_ = host_ips[0];
                                 acc->doRegister2_();
                             }
                         });
}

void
SIPAccount::doRegister2_()
{
    if (not isIP2IP() and not hostIp_) {
        setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_NOT_FOUND);
        JAMI_ERR("Hostname not resolved.");
        return;
    }

    IpAddr bindAddress = createBindingAddress();
    if (not bindAddress) {
        setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_NOT_FOUND);
        JAMI_ERR("Can't compute address to bind.");
        return;
    }

    bool ipv6 = bindAddress.isIpv6();
    transportType_ = tlsEnable_ ? (ipv6 ? PJSIP_TRANSPORT_TLS6 : PJSIP_TRANSPORT_TLS)
                                : (ipv6 ? PJSIP_TRANSPORT_UDP6 : PJSIP_TRANSPORT_UDP);

    // Init TLS settings if the user wants to use TLS
    if (tlsEnable_) {
        JAMI_DBG("TLS is enabled for account %s", accountID_.c_str());

        // Dropping current calls already using the transport is currently required
        // with TLS.
        hangupCalls();
        initTlsConfiguration();

        if (!tlsListener_) {
            tlsListener_ = link_.sipTransportBroker->getTlsListener(bindAddress, getTlsSetting());
            if (!tlsListener_) {
                setRegistrationState(RegistrationState::ERROR_GENERIC);
                JAMI_ERR("Error creating TLS listener.");
                return;
            }
        }
    } else {
        tlsListener_.reset();
    }

    // Init STUN settings for this account if the user selected it
    initStunConfiguration();

    // In our definition of the ip2ip profile (aka Direct IP Calls),
    // no registration should be performed
    if (isIP2IP()) {
        // If we use Tls for IP2IP, transports will be created on connection.
        if (!tlsEnable_) {
            setTransport(link_.sipTransportBroker->getUdpTransport(bindAddress));
        }
        setRegistrationState(RegistrationState::REGISTERED);
        return;
    }

    try {
        JAMI_WARN("Creating transport");
        transport_.reset();
        if (isTlsEnabled()) {
            setTransport(link_.sipTransportBroker->getTlsTransport(tlsListener_,
                                                                   hostIp_,
                                                                   tlsServerName_.empty()
                                                                       ? hostname_
                                                                       : tlsServerName_));
        } else {
            setTransport(link_.sipTransportBroker->getUdpTransport(bindAddress));
        }
        if (!transport_)
            throw VoipLinkException("Can't create transport");

        sendRegister();
    } catch (const VoipLinkException& e) {
        JAMI_ERR("%s", e.what());
        setRegistrationState(RegistrationState::ERROR_GENERIC);
        return;
    }

    if (presence_ and presence_->isEnabled()) {
        presence_->subscribeClient(getFromUri(), true); // self presence subscription
        presence_->sendPresence(true, "");              // try to publish whatever the status is.
    }
}

void
SIPAccount::doUnregister(std::function<void(bool)> released_cb)
{
    std::unique_lock<std::recursive_mutex> lock(configurationMutex_);

    tlsListener_.reset();

    if (!isIP2IP()) {
        try {
            sendUnregister();
        } catch (const VoipLinkException& e) {
            JAMI_ERR("doUnregister %s", e.what());
        }
    }

    if (transport_)
        setTransport();
    resetAutoRegistration();

    lock.unlock();
    if (released_cb)
        released_cb(not isIP2IP());
}

void
SIPAccount::connectivityChanged()
{
    if (not isUsable()) {
        // nothing to do
        return;
    }

    doUnregister([acc = shared()](bool /* transport_free */) {
        if (acc->isUsable())
            acc->doRegister();
    });
}

void
SIPAccount::sendRegister()
{
    if (not isUsable()) {
        JAMI_WARN("Account must be enabled and active to register, ignoring");
        return;
    }

    setRegister(true);
    setRegistrationState(RegistrationState::TRYING);

    pjsip_regc* regc = nullptr;
    if (pjsip_regc_create(link_.getEndpoint(), (void*) this, &registration_cb, &regc) != PJ_SUCCESS)
        throw VoipLinkException("UserAgent: Unable to create regc structure.");

    std::string srvUri(getServerUri());
    pj_str_t pjSrv {(char*) srvUri.data(), (pj_ssize_t) srvUri.size()};

    // Generate the FROM header
    std::string from(getFromUri());
    pj_str_t pjFrom(sip_utils::CONST_PJ_STR(from));

    // Get the received header
    const std::string& received(getReceivedParameter());

    std::string contact = getContactHeader();

    JAMI_DBG("Using contact header %s in registration", contact.c_str());

    if (transport_) {
        if (getUPnPActive() or not getPublishedSameasLocal()
            or (not received.empty() and received != getPublishedAddress())) {
            pjsip_host_port* via = getViaAddr();
            JAMI_DBG("Setting VIA sent-by to %.*s:%d",
                     (int) via->host.slen,
                     via->host.ptr,
                     via->port);

            if (pjsip_regc_set_via_sent_by(regc, via, transport_->get()) != PJ_SUCCESS)
                throw VoipLinkException("Unable to set the \"sent-by\" field");
        } else if (isStunEnabled()) {
            if (pjsip_regc_set_via_sent_by(regc, getViaAddr(), transport_->get()) != PJ_SUCCESS)
                throw VoipLinkException("Unable to set the \"sent-by\" field");
        }
    }

    pj_status_t status = PJ_SUCCESS;
    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);

    if ((status
         = pjsip_regc_init(regc, &pjSrv, &pjFrom, &pjFrom, 1, &pjContact, getRegistrationExpire()))
        != PJ_SUCCESS) {
        JAMI_ERR("pjsip_regc_init failed with error %d: %s",
                 status,
                 sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to initialize account registration structure");
    }

    if (hasServiceRoute())
        pjsip_regc_set_route_set(regc,
                                 sip_utils::createRouteSet(getServiceRoute(), link_.getPool()));

    pjsip_regc_set_credentials(regc, getCredentialCount(), getCredInfo());

    pjsip_hdr hdr_list;
    pj_list_init(&hdr_list);
    auto pjUserAgent = CONST_PJ_STR(getUserAgentName());
    constexpr pj_str_t STR_USER_AGENT = CONST_PJ_STR("User-Agent");

    pjsip_generic_string_hdr* h = pjsip_generic_string_hdr_create(link_.getPool(),
                                                                  &STR_USER_AGENT,
                                                                  &pjUserAgent);
    pj_list_push_back(&hdr_list, (pjsip_hdr*) h);
    pjsip_regc_add_headers(regc, &hdr_list);

    pjsip_tx_data* tdata;

    if (pjsip_regc_register(regc, isRegistrationRefreshEnabled(), &tdata) != PJ_SUCCESS)
        throw VoipLinkException("Unable to initialize transaction data for account registration");

    const pjsip_tpselector tp_sel = getTransportSelector();
    if (pjsip_regc_set_transport(regc, &tp_sel) != PJ_SUCCESS)
        throw VoipLinkException("Unable to set transport");

    setUpTransmissionData(tdata, tp_sel.u.transport->key.type);

    // pjsip_regc_send increment the transport ref count by one,
    if ((status = pjsip_regc_send(regc, tdata)) != PJ_SUCCESS) {
        JAMI_ERR("pjsip_regc_send failed with error %d: %s",
                 status,
                 sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to send account registration request");
    }

    setRegistrationInfo(regc);
}

void
SIPAccount::setUpTransmissionData(pjsip_tx_data* tdata, long transportKeyType)
{
    if (hostIp_) {
        auto ai = &tdata->dest_info;
        ai->name = pj_strdup3(tdata->pool, hostname_.c_str());
        ai->addr.count = 1;
        ai->addr.entry[0].type = (pjsip_transport_type_e) transportKeyType;
        pj_memcpy(&ai->addr.entry[0].addr, hostIp_.pjPtr(), sizeof(pj_sockaddr));
        ai->addr.entry[0].addr_len = hostIp_.getLength();
        ai->cur_addr = 0;
    }
}

void
SIPAccount::onRegister(pjsip_regc_cbparam* param)
{
    if (param->regc != getRegistrationInfo())
        return;

    if (param->status != PJ_SUCCESS) {
        JAMI_ERR("SIP registration error %d", param->status);
        destroyRegistrationInfo();
        setRegistrationState(RegistrationState::ERROR_GENERIC, param->code);
    } else if (param->code < 0 || param->code >= 300) {
        JAMI_ERR("SIP registration failed, status=%d (%.*s)",
                 param->code,
                 (int) param->reason.slen,
                 param->reason.ptr);
        destroyRegistrationInfo();
        switch (param->code) {
        case PJSIP_SC_FORBIDDEN:
            setRegistrationState(RegistrationState::ERROR_AUTH, param->code);
            break;
        case PJSIP_SC_NOT_FOUND:
            setRegistrationState(RegistrationState::ERROR_HOST, param->code);
            break;
        case PJSIP_SC_REQUEST_TIMEOUT:
            setRegistrationState(RegistrationState::ERROR_HOST, param->code);
            break;
        case PJSIP_SC_SERVICE_UNAVAILABLE:
            setRegistrationState(RegistrationState::ERROR_SERVICE_UNAVAILABLE, param->code);
            break;
        default:
            setRegistrationState(RegistrationState::ERROR_GENERIC, param->code);
        }
    } else if (PJSIP_IS_STATUS_IN_CLASS(param->code, 200)) {
        // Update auto registration flag
        resetAutoRegistration();

        if (param->expiration < 1) {
            destroyRegistrationInfo();
            JAMI_DBG("Unregistration success");
            setRegistrationState(RegistrationState::UNREGISTERED, param->code);
        } else {
            /* TODO Check and update SIP outbound status first, since the result
             * will determine if we should update re-registration
             */
            // update_rfc5626_status(acc, param->rdata);

            if (allowIPAutoRewrite_ and checkNATAddress(param, link_.getPool()))
                JAMI_WARN("New contact: %s", getContactHeader().c_str());

            /* TODO Check and update Service-Route header */
            if (hasServiceRoute())
                pjsip_regc_set_route_set(param->regc,
                                         sip_utils::createRouteSet(getServiceRoute(),
                                                                   link_.getPool()));

            setRegistrationState(RegistrationState::REGISTERED, param->code);
        }
    }

    /* Check if we need to auto retry registration. Basically, registration
     * failure codes triggering auto-retry are those of temporal failures
     * considered to be recoverable in relatively short term.
     */
    switch (param->code) {
    case PJSIP_SC_REQUEST_TIMEOUT:
    case PJSIP_SC_INTERNAL_SERVER_ERROR:
    case PJSIP_SC_BAD_GATEWAY:
    case PJSIP_SC_SERVICE_UNAVAILABLE:
    case PJSIP_SC_SERVER_TIMEOUT:
        scheduleReregistration();
        break;

    default:
        /* Global failure */
        if (PJSIP_IS_STATUS_IN_CLASS(param->code, 600))
            scheduleReregistration();
    }

    if (param->expiration != registrationExpire_) {
        JAMI_DBG("Registrar returned EXPIRE value [%u s] different from the requested [%u s]",
                 param->expiration,
                 registrationExpire_);
        // NOTE: We don't alter the EXPIRE set by the user even if the registrar
        // returned a different value. PJSIP lib will set the proper timer for
        // the refresh, if the auto-regisration is enabled.
    }
}

void
SIPAccount::sendUnregister()
{
    // This may occurs if account failed to register and is in state INVALID
    if (!isRegistered()) {
        setRegistrationState(RegistrationState::UNREGISTERED);
        return;
    }

    setRegister(false);

    pjsip_regc* regc = getRegistrationInfo();
    if (!regc)
        throw VoipLinkException("Registration structure is NULL");

    pjsip_tx_data* tdata = nullptr;
    if (pjsip_regc_unregister(regc, &tdata) != PJ_SUCCESS)
        throw VoipLinkException("Unable to unregister sip account");

    const pjsip_tpselector tp_sel = getTransportSelector();
    if (pjsip_regc_set_transport(regc, &tp_sel) != PJ_SUCCESS)
        throw VoipLinkException("Unable to set transport");

    setUpTransmissionData(tdata, tp_sel.u.transport->key.type);

    pj_status_t status;
    if ((status = pjsip_regc_send(regc, tdata)) != PJ_SUCCESS) {
        JAMI_ERR("pjsip_regc_send failed with error %d: %s",
                 status,
                 sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to send request to unregister sip account");
    }
}

pj_uint32_t
SIPAccount::tlsProtocolFromString(const std::string& method)
{
    if (method == "Default")
        return PJSIP_SSL_DEFAULT_PROTO;
    if (method == "TLSv1.2")
        return PJ_SSL_SOCK_PROTO_TLS1_2;
    if (method == "TLSv1.1")
        return PJ_SSL_SOCK_PROTO_TLS1_2 | PJ_SSL_SOCK_PROTO_TLS1_1;
    if (method == "TLSv1")
        return PJ_SSL_SOCK_PROTO_TLS1_2 | PJ_SSL_SOCK_PROTO_TLS1_1 | PJ_SSL_SOCK_PROTO_TLS1;
    return PJSIP_SSL_DEFAULT_PROTO;
}

/**
 * PJSIP aborts if our cipher list exceeds 1000 characters
 */
void
SIPAccount::trimCiphers()
{
    size_t sum = 0;
    unsigned count = 0;
    static const size_t MAX_CIPHERS_STRLEN = 1000;
    for (const auto& item : ciphers_) {
        sum += strlen(pj_ssl_cipher_name(item));
        if (sum > MAX_CIPHERS_STRLEN)
            break;
        ++count;
    }
    ciphers_.resize(count);
}

void
SIPAccount::initTlsConfiguration()
{
    pjsip_tls_setting_default(&tlsSetting_);
    tlsSetting_.proto = tlsProtocolFromString(tlsMethod_);

    // Determine the cipher list supported on this machine
    CipherArray avail_ciphers(256);
    unsigned cipherNum = avail_ciphers.size();
    if (pj_ssl_cipher_get_availables(&avail_ciphers.front(), &cipherNum) != PJ_SUCCESS)
        JAMI_ERR("Could not determine cipher list on this system");
    avail_ciphers.resize(cipherNum);

    ciphers_.clear();
    std::string_view stream(tlsCiphers_), item;
    while (jami::getline(stream, item, ' ')) {
        std::string cipher(item);
        auto item_cid = pj_ssl_cipher_id(cipher.c_str());
        if (item_cid != PJ_TLS_UNKNOWN_CIPHER) {
            JAMI_WARN("Valid cipher: %s", cipher.c_str());
            ciphers_.push_back(item_cid);
        } else
            JAMI_ERR("Invalid cipher: %s", cipher.c_str());
    }

    ciphers_.erase(std::remove_if(ciphers_.begin(),
                                  ciphers_.end(),
                                  [&](pj_ssl_cipher c) {
                                      return std::find(avail_ciphers.cbegin(),
                                                       avail_ciphers.cend(),
                                                       c)
                                             == avail_ciphers.cend();
                                  }),
                   ciphers_.end());

    trimCiphers();

    pj_strset(&tlsSetting_.ca_list_file, (char*) tlsCaListFile_.c_str(), tlsCaListFile_.size());
    pj_strset(&tlsSetting_.cert_file,
              (char*) tlsCertificateFile_.c_str(),
              tlsCertificateFile_.size());
    pj_strset(&tlsSetting_.privkey_file,
              (char*) tlsPrivateKeyFile_.c_str(),
              tlsPrivateKeyFile_.size());
    pj_strset(&tlsSetting_.password, (char*) tlsPassword_.c_str(), tlsPassword_.size());

    JAMI_DBG("Using %zu ciphers", ciphers_.size());
    tlsSetting_.ciphers_num = ciphers_.size();
    if (tlsSetting_.ciphers_num > 0) {
        tlsSetting_.ciphers = &ciphers_.front();
    }

    tlsSetting_.verify_server = tlsVerifyServer_;
    tlsSetting_.verify_client = tlsVerifyClient_;
    tlsSetting_.require_client_cert = tlsRequireClientCertificate_;

    tlsSetting_.timeout.sec = atol(tlsNegotiationTimeoutSec_.c_str());

    tlsSetting_.qos_type = PJ_QOS_TYPE_BEST_EFFORT;
    tlsSetting_.qos_ignore_error = PJ_TRUE;
}

void
SIPAccount::initStunConfiguration()
{
    std::string_view stunServer(stunServer_);
    auto pos = stunServer.find(':');
    if (pos == std::string_view::npos) {
        stunServerName_ = sip_utils::CONST_PJ_STR(stunServer);
        stunPort_ = PJ_STUN_PORT;
    } else {
        stunServerName_ = sip_utils::CONST_PJ_STR(stunServer.substr(0, pos));
        auto serverPort = stunServer.substr(pos + 1);
        std::from_chars(serverPort.data(), serverPort.data() + serverPort.size(), stunPort_);
    }
}

void
SIPAccount::loadConfig()
{
    if (registrationExpire_ == 0)
        registrationExpire_
            = DEFAULT_REGISTRATION_EXPIRE; /** Default expire value for registration */

    if (tlsEnable_) {
        initTlsConfiguration();
        transportType_ = PJSIP_TRANSPORT_TLS;
    } else
        transportType_ = PJSIP_TRANSPORT_UDP;
}

bool
SIPAccount::fullMatch(std::string_view username, std::string_view hostname) const
{
    return userMatch(username) and hostnameMatch(hostname);
}

bool
SIPAccount::userMatch(std::string_view username) const
{
    return !username.empty() and username == username_;
}

bool
SIPAccount::hostnameMatch(std::string_view hostname) const
{
    if (hostname == hostname_)
        return true;
    const auto a = ip_utils::getAddrList(hostname);
    const auto b = ip_utils::getAddrList(hostname_);
    return ip_utils::haveCommonAddr(a, b);
}

bool
SIPAccount::proxyMatch(std::string_view hostname) const
{
    if (hostname == serviceRoute_)
        return true;
    const auto a = ip_utils::getAddrList(hostname);
    const auto b = ip_utils::getAddrList(hostname_);
    return ip_utils::haveCommonAddr(a, b);
}

std::string
SIPAccount::getLoginName()
{
#ifndef _WIN32
    struct passwd* user_info = getpwuid(getuid());
    return user_info ? user_info->pw_name : "";
#elif defined(RING_UWP)
    return "Unknown";
#else
    DWORD size = UNLEN + 1;
    TCHAR username[UNLEN + 1];
    std::string uname;
    if (GetUserName((TCHAR*) username, &size)) {
        uname = jami::to_string(username);
    }
    return uname;
#endif
}

std::string
SIPAccount::getFromUri() const
{
    std::string scheme;
    std::string transport;

    // Get login name if username is not specified
    std::string username(username_.empty() ? getLoginName() : username_);
    std::string hostname(hostname_);

    // UDP does not require the transport specification
    if (transportType_ == PJSIP_TRANSPORT_TLS || transportType_ == PJSIP_TRANSPORT_TLS6) {
        scheme = "sips:";
        transport = ";transport=" + std::string(pjsip_transport_get_type_name(transportType_));
    } else
        scheme = "sip:";

    // Get machine hostname if not provided
    if (hostname_.empty()) {
        hostname = sip_utils::as_view(*pj_gethostname());
    }

    if (IpAddr::isIpv6(hostname))
        hostname = IpAddr(hostname).toString(false, true);

    std::string uri = "<" + scheme + username + "@" + hostname + transport + ">";
    if (not displayName_.empty())
        return "\"" + displayName_ + "\" " + uri;
    return uri;
}

std::string
SIPAccount::getToUri(const std::string& username) const
{
    std::string scheme;
    std::string transport;
    std::string hostname;

    // UDP does not require the transport specification
    if (transportType_ == PJSIP_TRANSPORT_TLS || transportType_ == PJSIP_TRANSPORT_TLS6) {
        scheme = "sips:";
        transport = ";transport=" + std::string(pjsip_transport_get_type_name(transportType_));
    } else
        scheme = "sip:";

    // Check if scheme is already specified
    if (username.find("sip") != std::string::npos)
        scheme = "";

    // Check if hostname is already specified
    if (username.find('@') == std::string::npos)
        hostname = hostname_;

    if (not hostname.empty() and IpAddr::isIpv6(hostname))
        hostname = IpAddr(hostname).toString(false, true);

    auto ltSymbol = username.find('<') == std::string::npos ? "<" : "";
    auto gtSymbol = username.find('>') == std::string::npos ? ">" : "";

    return ltSymbol + scheme + username + (hostname.empty() ? "" : "@") + hostname + transport
           + gtSymbol;
}

std::string
SIPAccount::getServerUri() const
{
    std::string scheme;
    std::string transport;

    // UDP does not require the transport specification
    if (transportType_ == PJSIP_TRANSPORT_TLS || transportType_ == PJSIP_TRANSPORT_TLS6) {
        scheme = "sips:";
        transport = ";transport=" + std::string(pjsip_transport_get_type_name(transportType_));
    } else {
        scheme = "sip:";
    }

    std::string host;
    if (IpAddr::isIpv6(hostname_))
        host = IpAddr(hostname_).toString(false, true);
    else
        host = hostname_;

    return "<" + scheme + host + transport + ">";
}

IpAddr
SIPAccount::getContactAddress() const
{
    std::lock_guard<std::mutex> lock(contactMutex_);
    return contactAddress_;
}

std::string
SIPAccount::getContactHeader() const
{
    std::lock_guard<std::mutex> lock(contactMutex_);
    return contactHeader_;
}

void
SIPAccount::updateContactHeader()
{
    std::lock_guard<std::mutex> lock(contactMutex_);

    if (not transport_ or not transport_->get()) {
        JAMI_ERR("Transport not created yet");
        return;
    }

    if (not contactAddress_) {
        JAMI_ERR("Invalid contact address: %s", contactAddress_.toString(true).c_str());
        return;
    }

    auto contactHdr = printContactHeader(username_,
                                         displayName_,
                                         contactAddress_.toString(false, true),
                                         contactAddress_.getPort(),
                                         PJSIP_TRANSPORT_IS_SECURE(transport_->get()),
                                         deviceKey_);

    contactHeader_ = std::move(contactHdr);
}

bool
SIPAccount::initContactAddress()
{
    // This method tries to determine the address to be used in the
    // contact header using the available information (current transport,
    // UPNP, STUN, ...). The contact address may be updated after the
    // registration using information sent by the registrar in the SIP
    // messages (see checkNATAddress).

    if (not transport_ or not transport_->get()) {
        JAMI_ERR("Transport not created yet");
        return {};
    }

    // The transport type must be specified, in our case START_OTHER refers to stun transport
    pjsip_transport_type_e transportType = transportType_;

    if (transportType == PJSIP_TRANSPORT_START_OTHER)
        transportType = PJSIP_TRANSPORT_UDP;

    std::string address;
    pj_uint16_t port;

    // Init the address to the local address.
    link_.findLocalAddressFromTransport(transport_->get(), transportType, hostname_, address, port);

    if (getUPnPActive() and getUPnPIpAddress()) {
        address = getUPnPIpAddress().toString();
        port = publishedPortUsed_;
        useUPnPAddressPortInVIA();
        JAMI_DBG("Using UPnP address %s and port %d", address.c_str(), port);
    } else if (not publishedSameasLocal_) {
        address = getPublishedIpAddress().toString();
        port = publishedPort_;
        JAMI_DBG("Using published address %s and port %d", address.c_str(), port);
    } else if (stunEnabled_) {
        auto success = link_.findLocalAddressFromSTUN(transport_->get(),
                                                      &stunServerName_,
                                                      stunPort_,
                                                      address,
                                                      port);
        if (not success)
            emitSignal<DRing::ConfigurationSignal::StunStatusFailed>(getAccountID());
        setPublishedAddress({address});
        publishedPort_ = port;
        usePublishedAddressPortInVIA();
    } else {
        if (!receivedParameter_.empty()) {
            address = receivedParameter_;
            JAMI_DBG("Using received address %s", address.c_str());
        }

        if (rPort_ > 0) {
            port = rPort_;
            JAMI_DBG("Using received port %d", port);
        }
    }

    std::lock_guard<std::mutex> lock(contactMutex_);
    contactAddress_ = IpAddr(address);
    contactAddress_.setPort(port);

    return contactAddress_;
}

std::string
SIPAccount::printContactHeader(const std::string& username,
                               const std::string& displayName,
                               const std::string& address,
                               pj_uint16_t port,
                               bool secure,
                               const std::string& deviceKey)
{
    // This method generates SIP contact header field, with push
    // notification parameters if any.
    // Example without push notification:
    // John Doe<sips:jdoe@10.10.10.10:5060;transport=tls>
    // Example with push notification:
    // John Doe<sips:jdoe@10.10.10.10:5060;transport=tls;pn-provider=XXX;pn-param=YYY;pn-prid=ZZZ>

    std::string quotedDisplayName = displayName.empty() ? "" : "\"" + displayName + "\" ";

    std::ostringstream contact;
    auto scheme = secure ? "sips" : "sip";
    auto transport = secure ? ";transport=tls" : "";

    contact << quotedDisplayName << "<" << scheme << ":" << username
            << (username.empty() ? "" : "@") << address << ":" << port << transport;

    if (not deviceKey.empty()) {
        contact
#if defined(__ANDROID__) || defined(ANDROID)
            << ";pn-provider=" << PN_FCM
#elif defined(__Apple__)
            << ";pn-provider=" << PN_APNS
#endif
            << ";pn-param="
            << ";pn-prid=" << deviceKey;
    }
    contact << ">";

    return contact.str();
}

pjsip_host_port
SIPAccount::getHostPortFromSTUN(pj_pool_t* pool)
{
    std::string addr;
    pj_uint16_t port;
    auto success = link_.findLocalAddressFromSTUN(transport_ ? transport_->get() : nullptr,
                                                  &stunServerName_,
                                                  stunPort_,
                                                  addr,
                                                  port);
    if (not success)
        emitSignal<DRing::ConfigurationSignal::StunStatusFailed>(getAccountID());
    pjsip_host_port result;
    pj_strdup2(pool, &result.host, addr.c_str());
    result.port = port;
    return result;
}

const std::vector<std::string>&
SIPAccount::getSupportedTlsCiphers()
{
    // Currently, both OpenSSL and GNUTLS implementations are static
    // reloading this for each account is unnecessary
    static std::vector<std::string> availCiphers {};

    // LIMITATION Assume the size might change, if there aren't any ciphers,
    // this will cause the cache to be repopulated at each call for nothing.
    if (availCiphers.empty()) {
        unsigned cipherNum = 256;
        CipherArray avail_ciphers(cipherNum);
        if (pj_ssl_cipher_get_availables(&avail_ciphers.front(), &cipherNum) != PJ_SUCCESS)
            JAMI_ERR("Could not determine cipher list on this system");
        avail_ciphers.resize(cipherNum);
        availCiphers.reserve(cipherNum);
        for (const auto& item : avail_ciphers) {
            if (item > 0) // 0 doesn't have a name
                availCiphers.push_back(pj_ssl_cipher_name(item));
        }
    }
    return availCiphers;
}

const std::vector<std::string>&
SIPAccount::getSupportedTlsProtocols()
{
    static std::vector<std::string> availProtos {VALID_TLS_PROTOS,
                                                 VALID_TLS_PROTOS + std::size(VALID_TLS_PROTOS)};
    return availProtos;
}

void
SIPAccount::Credentials::computePasswordHash()
{
    pj_md5_context pms;

    /* Compute md5 hash = MD5(username ":" realm ":" password) */
    pj_md5_init(&pms);
    pj_md5_update(&pms, (const uint8_t*) username.data(), username.length());
    pj_md5_update(&pms, (const uint8_t*) ":", 1);
    pj_md5_update(&pms, (const uint8_t*) realm.data(), realm.length());
    pj_md5_update(&pms, (const uint8_t*) ":", 1);
    pj_md5_update(&pms, (const uint8_t*) password.data(), password.length());

    unsigned char digest[16];
    pj_md5_final(&pms, digest);

    char hash[32];

    for (int i = 0; i < 16; ++i)
        pj_val_to_hex_digit(digest[i], &hash[2 * i]);

    password_h = {hash, 32};
}

void
SIPAccount::setCredentials(const std::vector<std::map<std::string, std::string>>& creds)
{
    if (creds.empty()) {
        JAMI_ERR("Cannot authenticate with empty credentials list");
        return;
    }
    credentials_.clear();
    cred_.clear();

    bool md5HashingEnabled = Manager::instance().preferences.getMd5Hash();

    credentials_.reserve(creds.size());
    cred_.reserve(creds.size());
    for (const auto& cred : creds) {
        auto realm = cred.find(Conf::CONFIG_ACCOUNT_REALM);
        auto user = cred.find(Conf::CONFIG_ACCOUNT_USERNAME);
        auto passw = cred.find(Conf::CONFIG_ACCOUNT_PASSWORD);
        credentials_.emplace_back(realm != cred.end() ? realm->second : "",
                                  user != cred.end() ? user->second : "",
                                  passw != cred.end() ? passw->second : "");
        auto& c = credentials_.back();
        if (md5HashingEnabled)
            c.computePasswordHash();

        cred_.emplace_back(
            pjsip_cred_info {/*.realm     = */ pj_str((char*) c.realm.c_str()),
                             /*.scheme    = */ pj_str((char*) "digest"),
                             /*.username  = */ pj_str((char*) c.username.c_str()),
                             /*.data_type = */
                             (c.password_h.empty() ? PJSIP_CRED_DATA_PLAIN_PASSWD
                                                   : PJSIP_CRED_DATA_DIGEST),
                             /*.data      = */
                             pj_str((char*) (c.password_h.empty() ? c.password.c_str()
                                                                  : c.password_h.c_str())),
                             /*.ext       = */ {}});
    }
}

std::vector<std::map<std::string, std::string>>
SIPAccount::getCredentials() const
{
    std::vector<std::map<std::string, std::string>> ret;
    ret.reserve(credentials_.size());
    for (const auto& c : credentials_) {
        ret.emplace_back(
            std::map<std::string, std::string> {{Conf::CONFIG_ACCOUNT_REALM, c.realm},
                                                {Conf::CONFIG_ACCOUNT_USERNAME, c.username},
                                                {Conf::CONFIG_ACCOUNT_PASSWORD, c.password}});
    }
    return ret;
}

void
SIPAccount::setRegistrationState(RegistrationState state,
                                 unsigned details_code,
                                 const std::string& /*detail_str*/)
{
    std::string details_str;
    const pj_str_t* description = pjsip_get_status_text(details_code);
    if (description)
        details_str = {description->ptr, (size_t) description->slen};
    setRegistrationStateDetailed({details_code, details_str});
    SIPAccountBase::setRegistrationState(state, details_code, details_str);
}

void
SIPAccount::setRegistrationExpire(unsigned expire)
{
    if (expire >= MIN_REGISTRATION_TIME) {
        JAMI_DBG("Set SIP registration EXPIRE to %u - current %u", expire, registrationExpire_);
        registrationExpire_ = expire;
    } else {
        JAMI_WARN("SIP registration EXPIRE %u is lower than min value %u",
                  expire,
                  MIN_REGISTRATION_TIME);
        registrationExpire_ = MIN_REGISTRATION_TIME;
    }
}

std::map<std::string, std::string>
SIPAccount::getTlsSettings() const
{
    return {{Conf::CONFIG_TLS_ENABLE, tlsEnable_ ? TRUE_STR : FALSE_STR},
            {Conf::CONFIG_TLS_LISTENER_PORT, std::to_string(tlsListenerPort_)},
            {Conf::CONFIG_TLS_CA_LIST_FILE, tlsCaListFile_},
            {Conf::CONFIG_TLS_CERTIFICATE_FILE, tlsCertificateFile_},
            {Conf::CONFIG_TLS_PRIVATE_KEY_FILE, tlsPrivateKeyFile_},
            {Conf::CONFIG_TLS_PASSWORD, tlsPassword_},
            {Conf::CONFIG_TLS_METHOD, tlsMethod_},
            {Conf::CONFIG_TLS_CIPHERS, tlsCiphers_},
            {Conf::CONFIG_TLS_SERVER_NAME, tlsServerName_},
            {Conf::CONFIG_TLS_VERIFY_SERVER, tlsVerifyServer_ ? TRUE_STR : FALSE_STR},
            {Conf::CONFIG_TLS_VERIFY_CLIENT, tlsVerifyClient_ ? TRUE_STR : FALSE_STR},
            {Conf::CONFIG_TLS_REQUIRE_CLIENT_CERTIFICATE,
             tlsRequireClientCertificate_ ? TRUE_STR : FALSE_STR},
            {Conf::CONFIG_TLS_NEGOTIATION_TIMEOUT_SEC, tlsNegotiationTimeoutSec_}};
}

bool
SIPAccount::isIP2IP() const
{
    return hostname_.empty();
}

SIPPresence*
SIPAccount::getPresence() const
{
    return presence_;
}

/**
 *  Enable the presence module
 */
void
SIPAccount::enablePresence(const bool& enabled)
{
    if (!presence_) {
        JAMI_ERR("Presence not initialized");
        return;
    }

    JAMI_DBG("Presence enabled for %s : %s.", accountID_.c_str(), enabled ? TRUE_STR : FALSE_STR);

    presence_->enable(enabled);
}

/**
 *  Set the presence (PUBLISH/SUBSCRIBE) support flags
 *  and process the change.
 */
void
SIPAccount::supportPresence(int function, bool enabled)
{
    if (!presence_) {
        JAMI_ERR("Presence not initialized");
        return;
    }

    if (presence_->isSupported(function) == enabled)
        return;

    JAMI_DBG("Presence support for %s (%s: %s).",
             accountID_.c_str(),
             function == PRESENCE_FUNCTION_PUBLISH ? "publish" : "subscribe",
             enabled ? TRUE_STR : FALSE_STR);
    presence_->support(function, enabled);

    // force presence to disable when nothing is supported
    if (not presence_->isSupported(PRESENCE_FUNCTION_PUBLISH)
        and not presence_->isSupported(PRESENCE_FUNCTION_SUBSCRIBE))
        enablePresence(false);

    Manager::instance().saveConfig();
    // FIXME: bad signal used here, we need a global config changed signal.
    emitSignal<DRing::ConfigurationSignal::AccountsChanged>();
}

MatchRank
SIPAccount::matches(std::string_view userName, std::string_view server) const
{
    if (fullMatch(userName, server)) {
        JAMI_DBG("Matching account id in request is a fullmatch %.*s@%.*s",
                 (int) userName.size(),
                 userName.data(),
                 (int) server.size(),
                 server.data());
        return MatchRank::FULL;
    } else if (hostnameMatch(server)) {
        JAMI_DBG("Matching account id in request with hostname %.*s",
                 (int) server.size(),
                 server.data());
        return MatchRank::PARTIAL;
    } else if (userMatch(userName)) {
        JAMI_DBG("Matching account id in request with username %.*s",
                 (int) userName.size(),
                 userName.data());
        return MatchRank::PARTIAL;
    } else if (proxyMatch(server)) {
        JAMI_DBG("Matching account id in request with proxy %.*s",
                 (int) server.size(),
                 server.data());
        return MatchRank::PARTIAL;
    } else {
        return MatchRank::NONE;
    }
}

void
SIPAccount::destroyRegistrationInfo()
{
    if (!regc_)
        return;
    pjsip_regc_destroy(regc_);
    regc_ = nullptr;
}

void
SIPAccount::resetAutoRegistration()
{
    auto_rereg_.active = PJ_FALSE;
    auto_rereg_.attempt_cnt = 0;
    if (auto_rereg_.timer.user_data) {
        delete ((std::weak_ptr<SIPAccount>*) auto_rereg_.timer.user_data);
        auto_rereg_.timer.user_data = nullptr;
    }
}

bool
SIPAccount::checkNATAddress(pjsip_regc_cbparam* param, pj_pool_t* pool)
{
    JAMI_DBG("[Account %s] Checking IP route after the registration", accountID_.c_str());

    pjsip_transport* tp = param->rdata->tp_info.transport;

    /* Get the received and rport info */
    pjsip_via_hdr* via = param->rdata->msg_info.via;
    int rport = 0;
    if (via->rport_param < 1) {
        /* Remote doesn't support rport */
        rport = via->sent_by.port;
        if (rport == 0) {
            pjsip_transport_type_e tp_type;
            tp_type = (pjsip_transport_type_e) tp->key.type;
            rport = pjsip_transport_get_default_port_for_type(tp_type);
        }
    } else {
        rport = via->rport_param;
    }

    const pj_str_t* via_addr = via->recvd_param.slen != 0 ? &via->recvd_param : &via->sent_by.host;
    std::string via_addrstr(sip_utils::as_view(*via_addr));
    /* Enclose IPv6 address in square brackets */
    if (IpAddr::isIpv6(via_addrstr))
        via_addrstr = IpAddr(via_addrstr).toString(false, true);

    JAMI_DBG("Checking received VIA address: %s", via_addrstr.c_str());

    if (via_addr_.host.slen == 0 or via_tp_ != tp) {
        if (pj_strcmp(&via_addr_.host, via_addr))
            pj_strdup(pool, &via_addr_.host, via_addr);

        // Update Via header
        via_addr_.port = rport;
        via_tp_ = tp;
        pjsip_regc_set_via_sent_by(regc_, &via_addr_, via_tp_);
    }

    // Set published Ip address
    publishedSameasLocal_ = false;
    publishedIpAddress_ = via_addrstr;
    setPublishedAddress(IpAddr(via_addrstr));

    /* Compare received and rport with the URI in our registration */
    IpAddr contact_addr = getContactAddress();

    // TODO. Why note save the port in contact uri/header?
    if (contact_addr.getPort() == 0) {
        pjsip_transport_type_e tp_type;
        tp_type = (pjsip_transport_type_e) tp->key.type;
        contact_addr.setPort(pjsip_transport_get_default_port_for_type(tp_type));
    }

    /* Convert IP address strings into sockaddr for comparison.
     * (http://trac.pjsip.org/repos/ticket/863)
     */
    bool matched = false;
    IpAddr recv_addr {};
    auto status = pj_sockaddr_parse(pj_AF_UNSPEC(), 0, via_addr, recv_addr.pjPtr());
    recv_addr.setPort(rport);
    if (status == PJ_SUCCESS) {
        // Compare the addresses as sockaddr according to the ticket above
        matched = contact_addr == recv_addr;
    } else {
        // Compare the addresses as string, as before
        auto pjContactAddr = sip_utils::CONST_PJ_STR(contact_addr.toString());
        matched = (contact_addr.getPort() == rport and pj_stricmp(&pjContactAddr, via_addr) == 0);
    }

    if (matched) {
        // Address doesn't change
        return false;
    }

    /* Get server IP */
    IpAddr srv_ip = {std::string_view(param->rdata->pkt_info.src_name)};

    /* At this point we've detected that the address as seen by registrar.
     * has changed.
     */

    /* Do not switch if both Contact and server's IP address are
     * public but response contains private IP. A NAT in the middle
     * might have messed up with the SIP packets. See:
     * http://trac.pjsip.org/repos/ticket/643
     *
     * This exception can be disabled by setting allow_contact_rewrite
     * to 2. In this case, the switch will always be done whenever there
     * is difference in the IP address in the response.
     */
    if (not contact_addr.isPrivate() and not srv_ip.isPrivate() and recv_addr.isPrivate()) {
        /* Don't switch */
        return false;
    }

    /* Also don't switch if only the port number part is different, and
     * the Via received address is private.
     * See http://trac.pjsip.org/repos/ticket/864
     */
    if (contact_addr == recv_addr and recv_addr.isPrivate()) {
        /* Don't switch */
        return false;
    }

    JAMI_WARN("[account %s] Contact address changed: "
              "(%s --> %s:%d). Updating registration.",
              accountID_.c_str(),
              contact_addr.toString(true).c_str(),
              via_addrstr.data(),
              rport);

    /*
     * Build new Contact header
     */
    {
        auto tempContact = printContactHeader(username_,
                                              displayName_,
                                              via_addrstr,
                                              rport,
                                              PJSIP_TRANSPORT_IS_SECURE(tp),
                                              deviceKey_);

        if (tempContact.empty()) {
            JAMI_ERR("Invalid contact header");
            return false;
        }

        // Update
        std::lock_guard<std::mutex> lock(contactMutex_);
        contactHeader_ = std::move(tempContact);
    }

    if (regc_ != nullptr) {
        auto contactHdr = getContactHeader();
        auto pjContact = sip_utils::CONST_PJ_STR(contactHdr);
        pjsip_regc_update_contact(regc_, 1, &pjContact);

        /*  Perform new registration at the next registration cycle */
    }

    return true;
}

/* Auto re-registration timeout callback */
void
SIPAccount::autoReregTimerCb()
{
    /* Check if the re-registration timer is still valid, e.g: while waiting
     * timeout timer application might have deleted the account or disabled
     * the auto-reregistration.
     */
    if (not auto_rereg_.active)
        return;

    /* Start re-registration */
    ++auto_rereg_.attempt_cnt;
    try {
        sendRegister();
    } catch (const VoipLinkException& e) {
        JAMI_ERR("Exception during SIP registration: %s", e.what());
        scheduleReregistration();
    }
}

/* Schedule reregistration for specified account. Note that the first
 * re-registration after a registration failure will be done immediately.
 * Also note that this function should be called within PJSUA mutex.
 */
void
SIPAccount::scheduleReregistration()
{
    if (!isUsable())
        return;

    /* Cancel any re-registration timer */
    if (auto_rereg_.timer.id) {
        auto_rereg_.timer.id = PJ_FALSE;
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &auto_rereg_.timer);
    }

    /* Update re-registration flag */
    auto_rereg_.active = PJ_TRUE;

    /* Set up timer for reregistration */
    auto_rereg_.timer.cb = [](pj_timer_heap_t* /*th*/, pj_timer_entry* te) {
        if (auto sipAccount = static_cast<std::weak_ptr<SIPAccount>*>(te->user_data)->lock())
            sipAccount->autoReregTimerCb();
    };
    if (not auto_rereg_.timer.user_data)
        auto_rereg_.timer.user_data = new std::weak_ptr<SIPAccount>(weak());

    /* Reregistration attempt. The first attempt will be done sooner */
    pj_time_val delay;
    delay.sec = auto_rereg_.attempt_cnt ? REGISTRATION_RETRY_INTERVAL
                                        : REGISTRATION_FIRST_RETRY_INTERVAL;
    delay.msec = 0;

    /* Randomize interval by +/- 10 secs */
    if (delay.sec >= 10) {
        delay.msec = delay10ZeroDist_(rand);
    } else {
        delay.sec = 0;
        delay.msec = delay10PosDist_(rand);
    }

    pj_time_val_normalize(&delay);

    JAMI_WARN("Scheduling re-registration retry in %ld seconds..", delay.sec);
    auto_rereg_.timer.id = PJ_TRUE;
    if (pjsip_endpt_schedule_timer(link_.getEndpoint(), &auto_rereg_.timer, &delay) != PJ_SUCCESS)
        auto_rereg_.timer.id = PJ_FALSE;
}

void
SIPAccount::updateDialogViaSentBy(pjsip_dialog* dlg)
{
    if (allowIPAutoRewrite_ && via_addr_.host.slen > 0)
        pjsip_dlg_set_via_sent_by(dlg, &via_addr_, via_tp_);
}

#if 0
/**
 * Create Accept header for MESSAGE.
 */
static pjsip_accept_hdr* im_create_accept(pj_pool_t *pool)
{
    /* Create Accept header. */
    pjsip_accept_hdr *accept;

    accept = pjsip_accept_hdr_create(pool);
    accept->values[0] = CONST_PJ_STR("text/plain");
    accept->values[1] = CONST_PJ_STR("application/im-iscomposing+xml");
    accept->count = 2;

    return accept;
}
#endif

void
SIPAccount::sendMessage(const std::string& to,
                        const std::map<std::string, std::string>& payloads,
                        uint64_t id,
                        bool,
                        bool)
{
    if (to.empty() or payloads.empty()) {
        JAMI_WARN("No sender or payload");
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    auto toUri = getToUri(to);

    constexpr pjsip_method msg_method = {PJSIP_OTHER_METHOD,
                                         CONST_PJ_STR(sip_utils::SIP_METHODS::MESSAGE)};
    std::string from(getFromUri());
    pj_str_t pjFrom = sip_utils::CONST_PJ_STR(from);
    pj_str_t pjTo = sip_utils::CONST_PJ_STR(toUri);

    /* Create request. */
    pjsip_tx_data* tdata;
    pj_status_t status = pjsip_endpt_create_request(link_.getEndpoint(),
                                                    &msg_method,
                                                    &pjTo,
                                                    &pjFrom,
                                                    &pjTo,
                                                    nullptr,
                                                    nullptr,
                                                    -1,
                                                    nullptr,
                                                    &tdata);
    if (status != PJ_SUCCESS) {
        JAMI_ERR("Unable to create request: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    /* Add Date Header. */
    pj_str_t date_str;
    constexpr auto key = CONST_PJ_STR("Date");
    pjsip_hdr* hdr;
    auto time = std::time(nullptr);
    auto date = std::ctime(&time);
    // the erase-remove idiom for a cstring, removes _all_ new lines with in date
    *std::remove(date, date + strlen(date), '\n') = '\0';

    // Add Header
    hdr = reinterpret_cast<pjsip_hdr*>(
        pjsip_date_hdr_create(tdata->pool, &key, pj_cstr(&date_str, date)));
    pjsip_msg_add_hdr(tdata->msg, hdr);

    // Add user-agent header
    sip_utils::addUserAgentHeader(getUserAgentName(), tdata);

    // Set input token into callback
    std::unique_ptr<ctx> t {new ctx(new pjsip_auth_clt_sess)};
    t->acc = shared();
    t->to = to;
    t->id = id;

    /* Initialize Auth header. */
    auto cred = getCredInfo();
    const_cast<pjsip_cred_info*>(cred)->realm = CONST_PJ_STR(hostname_);
    status = pjsip_auth_clt_init(t->auth_sess.get(), link_.getEndpoint(), tdata->pool, 0);

    if (status != PJ_SUCCESS) {
        JAMI_ERR("Unable to initialize auth session: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    status = pjsip_auth_clt_set_credentials(t->auth_sess.get(), getCredentialCount(), cred);

    if (status != PJ_SUCCESS) {
        JAMI_ERR("Unable to set auth session data: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    const pjsip_tpselector tp_sel = getTransportSelector();
    status = pjsip_tx_data_set_transport(tdata, &tp_sel);

    if (status != PJ_SUCCESS) {
        JAMI_ERR("Unable to set transport: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    im::fillPJSIPMessageBody(*tdata, payloads);

    // Send message request with callback SendMessageOnComplete
    status = pjsip_endpt_send_request(link_.getEndpoint(), tdata, -1, t.release(), &onComplete);

    if (status != PJ_SUCCESS) {
        JAMI_ERR("Unable to send request: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }
}

void
SIPAccount::onComplete(void* token, pjsip_event* event)
{
    std::unique_ptr<ctx> c {(ctx*) token};
    int code;
    pj_status_t status;
    pj_assert(event->type == PJSIP_EVENT_TSX_STATE);
    code = event->body.tsx_state.tsx->status_code;

    auto acc = c->acc.lock();
    if (not acc)
        return;

    // Check if Authorization Header if needed (request rejected by server)
    if (code == PJSIP_SC_UNAUTHORIZED || code == PJSIP_SC_PROXY_AUTHENTICATION_REQUIRED) {
        JAMI_INFO("Authorization needed for SMS message - Resending");
        pjsip_tx_data* new_request;

        // Add Authorization Header into msg
        status = pjsip_auth_clt_reinit_req(c->auth_sess.get(),
                                           event->body.tsx_state.src.rdata,
                                           event->body.tsx_state.tsx->last_tx,
                                           &new_request);

        if (status == PJ_SUCCESS) {
            // Increment Cseq number by one manually
            pjsip_cseq_hdr* cseq_hdr;
            cseq_hdr = (pjsip_cseq_hdr*) pjsip_msg_find_hdr(new_request->msg, PJSIP_H_CSEQ, NULL);
            cseq_hdr->cseq += 1;

            // Resend request
            auto to = c->to;
            auto id = c->id;
            status = pjsip_endpt_send_request(acc->link_.getEndpoint(),
                                              new_request,
                                              -1,
                                              c.release(),
                                              &onComplete);

            if (status != PJ_SUCCESS) {
                JAMI_ERR("Unable to send request: %s", sip_utils::sip_strerror(status).c_str());
                acc->messageEngine_.onMessageSent(to, id, false);
            }
            return;
        } else {
            JAMI_ERR("Unable to add Authorization Header into msg");
            acc->messageEngine_.onMessageSent(c->to, c->id, false);
            return;
        }
    }
    acc->messageEngine_.onMessageSent(c->to,
                                      c->id,
                                      event && event->body.tsx_state.tsx
                                          && (event->body.tsx_state.tsx->status_code == PJSIP_SC_OK
                                              || event->body.tsx_state.tsx->status_code
                                                     == PJSIP_SC_ACCEPTED));
}

std::string
SIPAccount::getUserUri() const
{
    return getFromUri();
}

IpAddr
SIPAccount::createBindingAddress()
{
    auto family = hostIp_ ? hostIp_.getFamily() : PJ_AF_INET;

    IpAddr ret = bindAddress_.empty()
                     ? (interface_ == ip_utils::DEFAULT_INTERFACE
                            ? ip_utils::getAnyHostAddr(family)
                            : ip_utils::getInterfaceAddr(getLocalInterface(), family))
                     : IpAddr(bindAddress_, family);

    if (ret.getPort() == 0) {
        ret.setPort(tlsEnable_ ? getTlsListenerPort() : getLocalPort());
    }

    return ret;
}

void
SIPAccount::setActiveCodecs(const std::vector<unsigned>& list)
{
    Account::setActiveCodecs(list);
    if (!hasActiveCodec(MEDIA_AUDIO)) {
        JAMI_WARN("All audio codecs disabled, enabling all");
        setAllCodecsActive(MEDIA_AUDIO, true);
    }
    if (!hasActiveCodec(MEDIA_VIDEO)) {
        JAMI_WARN("All video codecs disabled, enabling all");
        setAllCodecsActive(MEDIA_VIDEO, true);
    }
}

} // namespace jami
