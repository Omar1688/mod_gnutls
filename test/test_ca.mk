#!/usr/bin/make -f
# Authors:
# Daniel Kahn Gillmor <dkg@fifthhorseman.net>
# Thomas Klute <thomas2.klute@uni-dortmund.de>

# General rules to set up a miniature CA & server & client environment
# for the test suite

%.template: $(srcdir)/%.template.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@

%.uid: $(srcdir)/%.uid.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@

%/secret.key:
	mkdir -p $(dir $@)
	chmod 0700 $(dir $@)
	certtool --generate-privkey > $@

%/secring.gpg: %.uid %/secret.key
	rm -f $(dir $@)pubring.gpg $(dir $@)secring.gpg $(dir $@)trustdb.gpg
	PEM2OPENPGP_EXPIRATION=86400 PEM2OPENPGP_USAGE_FLAGS=authenticate,certify,sign pem2openpgp "$$(cat $<)" < $(dir $@)secret.key | GNUPGHOME=$(dir $@) gpg --import
	printf "%s:6:\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" | GNUPGHOME=$(dir $@) gpg --import-ownertrust

%/gpg.conf: %/secring.gpg
	printf "default-key %s\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/secret.pgp: %/secring.gpg
	GNUPGHOME=$(dir $@) gpg --armor --batch --no-tty --yes --export-secret-key "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/minimal.pgp: %/secring.gpg
	GNUPGHOME=$(dir $@) gpg --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

# Import and signing modify the shared keyring, which leads to race
# conditions with parallel make. Locking avoids this problem.
%/cert.pgp: %/minimal.pgp authority/gpg.conf
	GNUPGHOME=authority flock authority/lock gpg --import $<
	GNUPGHOME=authority flock authority/lock gpg --batch --sign-key --no-tty --yes "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)"
	GNUPGHOME=authority gpg --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

# special cases for the authorities' root certs:
authority/x509.pem: authority.template authority/secret.key
	certtool --generate-self-signed --load-privkey authority/secret.key --template authority.template > $@
rogueca/x509.pem: $(srcdir)/rogueca.template rogueca/secret.key
	certtool --generate-self-signed --load-privkey rogueca/secret.key --template $(srcdir)/rogueca.template > $@

%/cert-request: %.template %/secret.key 
	certtool --generate-request --load-privkey $(dir $@)secret.key --template $< > $@

%/x509.pem: %.template %/cert-request authority/secret.key authority/x509.pem 
	certtool --generate-certificate --load-ca-certificate authority/x509.pem --load-ca-privkey authority/secret.key --load-request $(dir $@)cert-request --template $< > $@

%/softhsm.db: %/x509.pem %/secret.key
	SOFTHSM_CONF="$(srcdir)/$(*)-softhsm.conf" $(srcdir)/softhsm.bash init $(dir $@)secret.key $(dir $@)x509.pem

# Generate CRL revoking a certain certificate. Currently used to
# revoke the server certificate and check if setting the CRL as
# GnuTLSProxyCRLFile causes the connection to the back end server to
# fail.
%/crl.pem: %/x509.pem ${srcdir}/%-crl.template
	certtool --generate-crl \
		--load-ca-privkey authority/secret.key \
		--load-ca-certificate authority/x509.pem \
		--load-certificate $< \
		--template "${srcdir}/$(*)-crl.template" \
		> $@
