/*
 *  Copyright (C) 2020-2022 Savoir-faire Linux Inc.
 *  Author: Sébastien Blin <sebastien.blin@savoirfairelinux.com>
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
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <condition_variable>
#include <filesystem>
#include <string>

#include "manager.h"
#include "jamidht/connectionmanager.h"
#include "jamidht/jamiaccount.h"
#include "sip/sipcall.h"
#include "sip/siptransport.h"
#include "../../test_runner.h"
#include "jami.h"
#include "account_const.h"

#include "common.h"

using namespace DRing::Account;

namespace jami {
namespace test {

class CallTest : public CppUnit::TestFixture
{
public:
    CallTest()
    {
        // Init daemon
        DRing::init(DRing::InitFlag(DRing::DRING_FLAG_DEBUG | DRing::DRING_FLAG_CONSOLE_LOG));
        if (not Manager::instance().initialized)
            CPPUNIT_ASSERT(DRing::start("jami-sample.yml"));
    }
    ~CallTest() { DRing::fini(); }
    static std::string name() { return "Call"; }
    void setUp();
    void tearDown();

    std::string aliceId;
    std::string bobId;
    std::string bob2Id;

private:
    void testCall();
    void testCachedCall();
    void testStopSearching();
    void testDeclineMultiDevice();
    void testTlsInfosPeerCertificate();

    CPPUNIT_TEST_SUITE(CallTest);
    CPPUNIT_TEST(testCall);
    CPPUNIT_TEST(testCachedCall);
    CPPUNIT_TEST(testStopSearching);
    CPPUNIT_TEST(testDeclineMultiDevice);
    CPPUNIT_TEST(testTlsInfosPeerCertificate);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(CallTest, CallTest::name());

void
CallTest::setUp()
{
    auto actors = load_actors_and_wait_for_announcement("actors/alice-bob.yml");
    aliceId = actors["alice"];
    bobId = actors["bob"];
}

void
CallTest::tearDown()
{
    auto bobArchive = std::filesystem::current_path().string() + "/bob.gz";
    std::remove(bobArchive.c_str());

    if (bob2Id.empty()) {
        wait_for_removal_of({aliceId, bobId});
    } else {
        wait_for_removal_of({aliceId, bobId, bob2Id});
    }
}

void
CallTest::testCall()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobAccount = Manager::instance().getAccount<JamiAccount>(bobId);
    auto bobUri = bobAccount->getUsername();
    auto aliceUri = aliceAccount->getUsername();

    std::mutex mtx;
    std::unique_lock<std::mutex> lk {mtx};
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<DRing::CallbackWrapperBase>> confHandlers;
    std::atomic_bool callReceived {false};
    std::atomic<int> callStopped {0};
    // Watch signals
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::IncomingCallWithMedia>(
        [&](const std::string&,
            const std::string&,
            const std::string&,
            const std::vector<std::map<std::string, std::string>>&) {
            callReceived = true;
            cv.notify_one();
        }));
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::StateChange>(
        [&](const std::string&, const std::string&, const std::string& state, signed) {
            if (state == "OVER") {
                callStopped += 1;
                if (callStopped == 2)
                    cv.notify_one();
            }
        }));
    DRing::registerSignalHandlers(confHandlers);

    JAMI_INFO("Start call between alice and Bob");
    auto call = DRing::placeCallWithMedia(aliceId, bobUri, {});

    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return callReceived.load(); }));

    JAMI_INFO("Stop call between alice and Bob");
    callStopped = 0;
    Manager::instance().hangupCall(aliceId, call);
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return callStopped == 2; }));
}

void
CallTest::testCachedCall()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobAccount = Manager::instance().getAccount<JamiAccount>(bobId);
    auto bobUri = bobAccount->getUsername();
    auto bobDeviceId = DeviceId(std::string(bobAccount->currentDeviceId()));
    auto aliceUri = aliceAccount->getUsername();

    std::mutex mtx;
    std::unique_lock<std::mutex> lk {mtx};
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<DRing::CallbackWrapperBase>> confHandlers;
    std::atomic_bool callReceived {false}, successfullyConnected {false};
    std::atomic<int> callStopped {0};
    // Watch signals
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::IncomingCallWithMedia>(
        [&](const std::string&,
            const std::string&,
            const std::string&,
            const std::vector<std::map<std::string, std::string>>&) {
            callReceived = true;
            cv.notify_one();
        }));
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::StateChange>(
        [&](const std::string&, const std::string&, const std::string& state, signed) {
            if (state == "OVER") {
                callStopped += 1;
                if (callStopped == 2)
                    cv.notify_one();
            }
        }));
    DRing::registerSignalHandlers(confHandlers);

    JAMI_INFO("Connect Alice's device and Bob's device");
    aliceAccount->connectionManager()
        .connectDevice(bobDeviceId,
                       "sip",
                       [&cv, &successfullyConnected](std::shared_ptr<ChannelSocket> socket,
                                                     const DeviceId&) {
                           if (socket)
                               successfullyConnected = true;
                           cv.notify_one();
                       });
    CPPUNIT_ASSERT(
        cv.wait_for(lk, std::chrono::seconds(30), [&] { return successfullyConnected.load(); }));

    JAMI_INFO("Start call between alice and Bob");
    auto call = DRing::placeCallWithMedia(aliceId, bobUri, {});
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return callReceived.load(); }));

    callStopped = 0;
    JAMI_INFO("Stop call between alice and Bob");
    Manager::instance().hangupCall(aliceId, call);
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return callStopped == 2; }));
}

void
CallTest::testStopSearching()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobAccount = Manager::instance().getAccount<JamiAccount>(bobId);
    auto bobUri = bobAccount->getUsername();
    auto aliceUri = aliceAccount->getUsername();

    Manager::instance().sendRegister(bobId, false);

    std::mutex mtx;
    std::unique_lock<std::mutex> lk {mtx};
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<DRing::CallbackWrapperBase>> confHandlers;
    std::atomic_bool callStopped {false};
    // Watch signals
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::StateChange>(
        [&](const std::string&, const std::string&, const std::string& state, signed) {
            if (state == "OVER") {
                callStopped = true;
                cv.notify_one();
            }
        }));
    DRing::registerSignalHandlers(confHandlers);

    JAMI_INFO("Start call between alice and Bob");
    auto call = DRing::placeCallWithMedia(aliceId, bobUri, {});

    // Bob not there, so we should get a SEARCHING STATUS
    JAMI_INFO("Wait OVER state");
    // Then wait for the DHT no answer. this can take some times
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(60), [&] { return callStopped.load(); }));
}

void
CallTest::testDeclineMultiDevice()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobAccount = Manager::instance().getAccount<JamiAccount>(bobId);
    auto bobUri = bobAccount->getUsername();
    auto aliceUri = aliceAccount->getUsername();
    std::mutex mtx;
    std::unique_lock<std::mutex> lk {mtx};
    std::condition_variable cv;

    // Add second device for Bob
    std::map<std::string, std::shared_ptr<DRing::CallbackWrapperBase>> confHandlers;
    auto bobArchive = std::filesystem::current_path().string() + "/bob.gz";
    std::remove(bobArchive.c_str());
    bobAccount->exportArchive(bobArchive);

    std::map<std::string, std::string> details = DRing::getAccountTemplate("RING");
    details[ConfProperties::TYPE] = "RING";
    details[ConfProperties::DISPLAYNAME] = "BOB2";
    details[ConfProperties::ALIAS] = "BOB2";
    details[ConfProperties::UPNP_ENABLED] = "true";
    details[ConfProperties::ARCHIVE_PASSWORD] = "";
    details[ConfProperties::ARCHIVE_PIN] = "";
    details[ConfProperties::ARCHIVE_PATH] = bobArchive;

    bob2Id = Manager::instance().addAccount(details);

    wait_for_announcement_of(bob2Id);

    std::atomic<int> callReceived {0};
    std::atomic<int> callStopped {0};
    std::string callIdBob;
    // Watch signals
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::IncomingCallWithMedia>(
        [&](const std::string& accountId,
            const std::string& callId,
            const std::string&,
            const std::vector<std::map<std::string, std::string>>&) {
            if (accountId == bobId)
                callIdBob = callId;
            callReceived += 1;
            cv.notify_one();
        }));
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::StateChange>(
        [&](const std::string&, const std::string&, const std::string& state, signed) {
            if (state == "OVER")
                callStopped++;
            cv.notify_one();
        }));
    DRing::registerSignalHandlers(confHandlers);

    JAMI_INFO("Start call between alice and Bob");
    auto call = DRing::placeCallWithMedia(aliceId, bobUri, {});

    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] {
        return callReceived == 2 && !callIdBob.empty();
    }));

    JAMI_INFO("Stop call between alice and Bob");
    callStopped = 0;
    Manager::instance().refuseCall(bobId, callIdBob);
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] {
        return callStopped.load() >= 3; /* >= because there is subcalls */
    }));
}

void
CallTest::testTlsInfosPeerCertificate()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobAccount = Manager::instance().getAccount<JamiAccount>(bobId);
    auto bobUri = bobAccount->getUsername();
    auto aliceUri = aliceAccount->getUsername();

    std::mutex mtx;
    std::unique_lock<std::mutex> lk {mtx};
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<DRing::CallbackWrapperBase>> confHandlers;
    std::atomic<int> callStopped {0};
    std::string bobCallId;
    std::string aliceCallState;
    // Watch signals
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::IncomingCallWithMedia>(
        [&](const std::string& accountId,
            const std::string& callId,
            const std::string&,
            const std::vector<std::map<std::string, std::string>>&) {
            if (accountId == bobId)
                bobCallId = callId;
            cv.notify_one();
        }));
    confHandlers.insert(DRing::exportable_callback<DRing::CallSignal::StateChange>(
        [&](const std::string& accountId, const std::string&, const std::string& state, signed) {
            if (accountId == aliceId)
                aliceCallState = state;
            if (state == "OVER") {
                callStopped += 1;
                if (callStopped == 2)
                    cv.notify_one();
            }
        }));
    DRing::registerSignalHandlers(confHandlers);

    JAMI_INFO("Start call between alice and Bob");
    auto callId = DRing::placeCallWithMedia(aliceId, bobUri, {});

    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return !bobCallId.empty(); }));

    Manager::instance().answerCall(bobId, bobCallId);
    CPPUNIT_ASSERT(
        cv.wait_for(lk, std::chrono::seconds(30), [&] { return aliceCallState == "CURRENT"; }));

    auto call = std::dynamic_pointer_cast<SIPCall>(aliceAccount->getCall(callId));
    auto* transport = call->getTransport();
    CPPUNIT_ASSERT(transport);
    auto cert = transport->getTlsInfos().peerCert;
    CPPUNIT_ASSERT(cert && cert->issuer);
    CPPUNIT_ASSERT(cert->issuer->getId().toString() == bobAccount->getUsername());

    JAMI_INFO("Stop call between alice and Bob");
    callStopped = 0;
    Manager::instance().hangupCall(aliceId, callId);
    CPPUNIT_ASSERT(cv.wait_for(lk, std::chrono::seconds(30), [&] { return callStopped == 2; }));
}

} // namespace test
} // namespace jami

RING_TEST_RUNNER(jami::test::CallTest::name())
