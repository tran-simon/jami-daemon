# OPENDHT
OPENDHT_VERSION := af7d88b057fa4c84ab9096c6a1932bd5d34634ef
OPENDHT_URL := https://github.com/savoirfairelinux/opendht/archive/$(OPENDHT_VERSION).tar.gz

PKGS += opendht
ifeq ($(call need_pkg,'opendht >= 2.4.8'),)
PKGS_FOUND += opendht
endif

# Avoid building distro-provided dependencies in case opendht was built manually
ifneq ($(call need_pkg,"msgpack >= 1.2"),)
DEPS_opendht += msgpack
endif
ifneq ($(call need_pkg,"libargon2"),)
DEPS_opendht += argon2
endif
ifneq ($(and $(call need_pkg,"openssl >= 1.1.0"),$(call need_pkg,"libressl >= 1.12.2")),)
DEPS_opendht += libressl
endif
ifneq ($(call need_pkg,"restinio >= v.0.6.16"),)
DEPS_opendht += restinio
endif
ifneq ($(call need_pkg,"jsoncpp >= 1.7.2"),)
DEPS_opendht += jsoncpp
endif
ifneq ($(call need_pkg,"gnutls >= 3.3.0"),)
DEPS_opendht += gnutls
endif

# fmt 5.3.0 fix: https://github.com/fmtlib/fmt/issues/1267
OPENDHT_CONF = FMT_USE_USER_DEFINED_LITERALS=0

$(TARBALLS)/opendht-$(OPENDHT_VERSION).tar.gz:
	$(call download,$(OPENDHT_URL))

.sum-opendht: opendht-$(OPENDHT_VERSION).tar.gz

opendht: opendht-$(OPENDHT_VERSION).tar.gz
	$(UNPACK)
	$(UPDATE_AUTOCONFIG) && cd $(UNPACK_DIR)
	$(MOVE)

.opendht: opendht .sum-opendht
	mkdir -p $</m4 && $(RECONF)
	cd $< && $(HOSTVARS) $(OPENDHT_CONF) ./configure --enable-static --disable-shared --disable-c --disable-tools --disable-indexation --disable-python --disable-doc --enable-proxy-server --enable-proxy-client --enable-push-notifications $(HOSTCONF)
	cd $< && $(MAKE) install
	touch $@
