#!/usr/bin/make -f
# Authors:
# Daniel Kahn Gillmor <dkg@fifthhorseman.net>
# Fiona Klute <fiona.klute@gmx.de>

# General rules to set up a miniature CA & server & client environment
# for the test suite

%.template: $(srcdir)/%.template.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@
	sed -i -e "s,__OCSP_URI__,$(OCSP_URI_TEMPLATE)," $@
	for i in $(patsubst [%],%,$(TEST_IP)); do \
		IP_ADDRS="$${IP_ADDRS}\nip_address = $${i}"; \
	done; \
	sed -i -e "s,__IP_ADDRESSES__,$${IP_ADDRS#\\n}," $@

%/uid: $(srcdir)/%/uid.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@

%/secret.key:
	mkdir -p $(dir $@)
	chmod 0700 $(dir $@)
	certtool --outfile $@ --generate-privkey

.PRECIOUS: %/secret.key

%/secret.pgp.raw: %/uid %/secret.key
	PEM2OPENPGP_USAGE_FLAGS=authenticate,certify,sign pem2openpgp "$$(cat $<)" < $(dir $@)secret.key > $@

%/secret.pgp: %/secret.pgp.raw pgpcrc
	(printf -- '-----BEGIN PGP PRIVATE KEY BLOCK-----\nVersion: test\n\n' && \
	base64 < $< && \
	printf -- '=' && \
	./pgpcrc < $< | base64 && \
	printf -- '-----END PGP PRIVATE KEY BLOCK-----\n' ) > $@

%/gpg.conf: %/secret.pgp
	rm -f $(dir $@)pubring.gpg $(dir $@)secring.gpg $(dir $@)trustdb.gpg $(dir $@)pubring.kbx $(dir $@)private-keys-v1.d/*.key
	GNUPGHOME=$(dir $@) gpg --import $<
	printf "%s:6:\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" | GNUPGHOME=$(dir $@) gpg --import-ownertrust
	printf "default-key %s\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/minimal.pgp: %/gpg.conf
	if test -r $@; then rm $@; fi
	GNUPGHOME=$(dir $@) gpg --output $@ --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)"

# Import and signing modify the shared keyring, which leads to race
# conditions with parallel make. Locking avoids this problem. Building
# authority/minimal.pgp (instead of just authority/gpg.conf) before
# */cert.pgp avoids having to lock for all */minimal.pgp, too.
%/cert.pgp: %/minimal.pgp authority/minimal.pgp
	if test -r $@; then rm $@; fi
	GNUPGHOME=authority/ $(GPG_FLOCK) gpg --import $<
	GNUPGHOME=authority/ $(GPG_FLOCK) gpg --batch --sign-key --no-tty --yes "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)"
	GNUPGHOME=authority/ $(GPG_FLOCK) gpg --output $@ --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)"

# special cases for the authorities' root certs:
authority/x509.pem: authority.template authority/secret.key
	certtool --outfile $@ --generate-self-signed --load-privkey authority/secret.key --template authority.template
rogueca/x509.pem: $(srcdir)/rogueca.template rogueca/secret.key
	certtool --outfile $@ --generate-self-signed --load-privkey rogueca/secret.key --template $(srcdir)/rogueca.template

# normal case: certificates signed by test CA
%/x509.pem: %.template %/secret.key authority/secret.key authority/x509.pem
	certtool --outfile $@ --generate-certificate --load-ca-certificate authority/x509.pem --load-ca-privkey authority/secret.key --load-privkey $(dir $@)secret.key --template $<

# error case: certificates signed by rogue CA
rogue%/x509.pem: rogue%.template rogue%/secret.key rogueca/x509.pem
	certtool --outfile $@ --generate-certificate --load-ca-certificate rogueca/x509.pem --load-ca-privkey rogueca/secret.key --load-privkey $(dir $@)secret.key --template $<

%/softhsm.conf: %/secret.key
	echo "0:$(dir $@)softhsm.db" > $@

%/softhsm.db: %/x509.pem %/secret.key %/softhsm.conf
	SOFTHSM="$(SOFTHSM)" \
	SOFTHSM_CONF="$(dir $@)softhsm.conf" \
	$(srcdir)/softhsm.bash init $(dir $@)secret.key $(dir $@)x509.pem

%/softhsm2.conf: %/secret.key
	echo "objectstore.backend = file" > $@
	echo "directories.tokendir = $(dir $@)softhsm2.db" >> $@

%/softhsm2.db: %/x509.pem %/secret.key %/softhsm2.conf
	rm -rf $@
	mkdir -p $@
	SOFTHSM="$(SOFTHSM)" \
	SOFTHSM2_CONF="$(dir $@)softhsm2.conf" \
	$(srcdir)/softhsm.bash init $(dir $@)secret.key $(dir $@)x509.pem

# Generate CRL revoking a certain certificate. Currently used to
# revoke the server certificate and check if setting the CRL as
# GnuTLSProxyCRLFile causes the connection to the back end server to
# fail.
%/crl.pem: %/x509.pem ${srcdir}/%-crl.template
	certtool --generate-crl \
		--outfile $@ \
		--load-ca-privkey authority/secret.key \
		--load-ca-certificate authority/x509.pem \
		--load-certificate $< \
		--template "${srcdir}/$(*)-crl.template"
