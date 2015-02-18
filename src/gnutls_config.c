/**
 *  Copyright 2004-2005 Paul Querna
 *  Copyright 2008 Nikos Mavrogiannopoulos
 *  Copyright 2011 Dash Shendy
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "mod_gnutls.h"
#include "apr_lib.h"
#include <gnutls/abstract.h>

#define INIT_CA_SIZE 128

#ifdef APLOG_USE_MODULE
APLOG_USE_MODULE(gnutls);
#endif

static int pin_callback(void *user, int attempt, const char *token_url,
			const char *token_label, unsigned int flags,
			char *pin, size_t pin_max)
{
    mgs_srvconf_rec *sc = user;

    if (sc->pin == NULL || flags & GNUTLS_PIN_FINAL_TRY ||
	flags & GNUTLS_PIN_WRONG) {
	return -1;
    }

    if (token_label && strcmp(token_label, "SRK") == 0) {
	 snprintf(pin, pin_max, "%s", sc->srk_pin);
    } else {
         snprintf(pin, pin_max, "%s", sc->pin);
    }
    return 0;
}

static int load_datum_from_file(apr_pool_t * pool,
				const char *file, gnutls_datum_t * data)
{
    apr_file_t *fp;
    apr_finfo_t finfo;
    apr_status_t rv;
    apr_size_t br = 0;

    rv = apr_file_open(&fp, file, APR_READ | APR_BINARY,
		       APR_OS_DEFAULT, pool);
    if (rv != APR_SUCCESS) {
	return rv;
    }

    rv = apr_file_info_get(&finfo, APR_FINFO_SIZE, fp);

    if (rv != APR_SUCCESS) {
	return rv;
    }

    data->data = apr_palloc(pool, finfo.size + 1);
    rv = apr_file_read_full(fp, data->data, finfo.size, &br);

    if (rv != APR_SUCCESS) {
	return rv;
    }
    apr_file_close(fp);

    data->data[br] = '\0';
    data->size = br;

    return 0;
}

/* 2048-bit group parameters from SRP specification */
const char static_dh_params[] = "-----BEGIN DH PARAMETERS-----\n"
        "MIIBBwKCAQCsa9tBMkqam/Fm3l4TiVgvr3K2ZRmH7gf8MZKUPbVgUKNzKcu0oJnt\n"
        "gZPgdXdnoT3VIxKrSwMxDc1/SKnaBP1Q6Ag5ae23Z7DPYJUXmhY6s2YaBfvV+qro\n"
        "KRipli8Lk7hV+XmT7Jde6qgNdArb9P90c1nQQdXDPqcdKB5EaxR3O8qXtDoj+4AW\n"
        "dr0gekNsZIHx0rkHhxdGGludMuaI+HdIVEUjtSSw1X1ep3onddLs+gMs+9v1L7N4\n"
        "YWAnkATleuavh05zA85TKZzMBBx7wwjYKlaY86jQw4JxrjX46dv7tpS1yAPYn3rk\n"
        "Nd4jbVJfVHWbZeNy/NaO8g+nER+eSv9zAgEC\n"
        "-----END DH PARAMETERS-----\n";

int mgs_load_files(apr_pool_t * p, server_rec * s)
{
    apr_pool_t *spool;
    const char *file;
    gnutls_datum_t data;
    int ret;
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						 &gnutls_module);

    apr_pool_create(&spool, p);

    sc->cert_pgp = apr_pcalloc(p, sizeof(sc->cert_pgp[0]));
    sc->cert_crt_pgp = apr_pcalloc(p, sizeof(sc->cert_crt_pgp[0]));
    sc->certs_x509_chain =
	apr_pcalloc(p, MAX_CHAIN_SIZE * sizeof(sc->certs_x509_chain[0]));
    sc->certs_x509_crt_chain =
	apr_pcalloc(p,
		    MAX_CHAIN_SIZE * sizeof(sc->certs_x509_crt_chain[0]));

    ret = gnutls_certificate_allocate_credentials(&sc->certs);
    if (ret < 0) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
		     "GnuTLS: Failed to initialize" ": (%d) %s", ret,
		     gnutls_strerror(ret));
	ret = -1;
	goto cleanup;
    }

    ret = gnutls_anon_allocate_server_credentials(&sc->anon_creds);
    if (ret < 0) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
		     "GnuTLS: Failed to initialize" ": (%d) %s", ret,
		     gnutls_strerror(ret));
	ret = -1;
	goto cleanup;
    }

    /* Load SRP parameters */
#ifdef ENABLE_SRP
    ret = gnutls_srp_allocate_server_credentials(&sc->srp_creds);
    if (ret < 0) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
		     "GnuTLS: Failed to initialize" ": (%d) %s", ret,
		     gnutls_strerror(ret));
	ret = -1;
	goto cleanup;
    }

    if (sc->srp_tpasswd_conf_file != NULL && sc->srp_tpasswd_file != NULL) {
	ret = gnutls_srp_set_server_credentials_file
	    (sc->srp_creds, sc->srp_tpasswd_file,
	     sc->srp_tpasswd_conf_file);

	if (ret < 0 && sc->enabled == GNUTLS_ENABLED_TRUE) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0,
			 s,
			 "GnuTLS: Host '%s:%d' is missing a "
			 "SRP password or conf File!",
			 s->server_hostname, s->port);
	    ret = -1;
	    goto cleanup;
	}
    }
#endif

    ret = gnutls_dh_params_init(&sc->dh_params);
    if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to initialize"
			 ": (%d) %s", ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
    }

    /* Load DH parameters */
    if (sc->dh_file) {
	if (load_datum_from_file(spool, sc->dh_file, &data) != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Error Reading " "DH params '%s'", sc->dh_file);
	    ret = -1;
	    goto cleanup;
	}


	ret =
	    gnutls_dh_params_import_pkcs3(sc->dh_params, &data,
					  GNUTLS_X509_FMT_PEM);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Import "
			 "DH params '%s': (%d) %s", sc->dh_file, ret,
			 gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}
    } else {
        gnutls_datum_t pdata = {
            (void *) static_dh_params,
            sizeof(static_dh_params)
        };

        ret = gnutls_dh_params_import_pkcs3(sc->dh_params, &pdata, GNUTLS_X509_FMT_PEM);
        if (ret < 0) {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
                    "GnuTLS: Unable to generate or load DH Params: (%d) %s",
                    ret, gnutls_strerror(ret));
            ret = -1;
            goto cleanup;
        }
    }

    if (sc->x509_cert_file != NULL) {
	unsigned int chain_num, i;
	unsigned format = GNUTLS_X509_FMT_PEM;

	/* Load X.509 certificate */
	if (strncmp(sc->x509_cert_file, "pkcs11:", 7) == 0) {
	    gnutls_pkcs11_obj_t obj;

	    file = sc->x509_cert_file;

	    ret = gnutls_pkcs11_obj_init(&obj);
	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Error Initializing PKCS #11 object");
		ret = -1;
		goto cleanup;
	    }

	    gnutls_pkcs11_obj_set_pin_function(obj, pin_callback, sc);

	    ret = gnutls_pkcs11_obj_import_url(obj, file, GNUTLS_PKCS11_OBJ_FLAG_LOGIN);
	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Error Importing PKCS #11 object: '%s': %s",
			     file, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }

	    format = GNUTLS_X509_FMT_DER;
	    ret = gnutls_pkcs11_obj_export2(obj, &data);
	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Error Exporting a PKCS #11 object: '%s': %s",
			     file, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }

	    gnutls_pkcs11_obj_deinit(obj);
	} else {
	    file = ap_server_root_relative(spool, sc->x509_cert_file);

	    ret = gnutls_load_file(file, &data);
	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Error Reading Certificate '%s': %s",
			     file, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }
	}

	ret =
	    gnutls_x509_crt_list_import2(&sc->certs_x509_crt_chain,
					&chain_num, &data, format,
					GNUTLS_X509_CRT_LIST_FAIL_IF_UNSORTED);
	gnutls_free(data.data);
	sc->certs_x509_chain_num = chain_num;

	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Import Certificate Chain '%s': (%d) %s",
			 file, ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	for (i = 0; i < chain_num; i++) {
	    ret =
		gnutls_pcert_import_x509(&sc->certs_x509_chain[i],
					 sc->certs_x509_crt_chain[i], 0);
	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Failed to Import pCertificate '%s': (%d) %s",
			     file, ret, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }
	}
	sc->certs_x509_chain_num = chain_num;
    }

    if (sc->x509_key_file) {
	ret = gnutls_privkey_init(&sc->privkey_x509);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to initialize: (%d) %s", ret,
			 gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	if (gnutls_url_is_supported(sc->x509_key_file) != 0) {
	    file = sc->x509_key_file;

	    gnutls_privkey_set_pin_function(sc->privkey_x509, pin_callback,
					    sc);

	    ret = gnutls_privkey_import_url(sc->privkey_x509, file, 0);

	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Failed to Import Private Key URL '%s': (%d) %s",
			     file, ret, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }
	} else {
	    file = ap_server_root_relative(spool, sc->x509_key_file);

	    if (load_datum_from_file(spool, file, &data) != 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Error Reading Private Key '%s'",
			     file);
		ret = -1;
		goto cleanup;
	    }

	    ret =
		gnutls_privkey_import_x509_raw(sc->privkey_x509, &data,
					       GNUTLS_X509_FMT_PEM, sc->pin,
					       0);

	    if (ret < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Failed to Import Private Key '%s': (%d) %s",
			     file, ret, gnutls_strerror(ret));
		ret = -1;
		goto cleanup;
	    }
	}
    }

    /* Load the X.509 CA file */
    if (sc->x509_ca_file) {
	if (load_datum_from_file(spool, sc->x509_ca_file, &data) != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Error Reading " "Client CA File '%s'",
			 sc->x509_ca_file);
	    ret = -1;
	    goto cleanup;
	}

	ret = gnutls_x509_crt_list_import2(&sc->ca_list, &sc->ca_list_size,
					 &data, GNUTLS_X509_FMT_PEM, 0);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to load "
			 "Client CA File '%s': (%d) %s", sc->x509_ca_file,
			 ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}
    }

    if (sc->pgp_cert_file) {
	if (load_datum_from_file(spool, sc->pgp_cert_file, &data) != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Error Reading " "Certificate '%s'",
			 sc->pgp_cert_file);
	    ret = -1;
	    goto cleanup;
	}

	ret = gnutls_openpgp_crt_init(&sc->cert_crt_pgp[0]);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Init "
			 "PGP Certificate: (%d) %s", ret,
			 gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	ret =
	    gnutls_openpgp_crt_import(sc->cert_crt_pgp[0], &data,
				      GNUTLS_OPENPGP_FMT_BASE64);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Import "
			 "PGP Certificate: (%d) %s", ret,
			 gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	ret =
	    gnutls_pcert_import_openpgp(sc->cert_pgp, sc->cert_crt_pgp[0],
					0);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Import "
			 "PGP pCertificate: (%d) %s", ret,
			 gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}
    }

    /* Load the PGP key file */
    if (sc->pgp_key_file) {
	if (load_datum_from_file(spool, sc->pgp_key_file, &data) != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Error Reading " "Private Key '%s'",
			 sc->pgp_key_file);
	    ret = -1;
	    goto cleanup;
	}

	ret = gnutls_privkey_init(&sc->privkey_pgp);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to initialize"
			 ": (%d) %s", ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	ret =
	    gnutls_privkey_import_openpgp_raw(sc->privkey_pgp, &data,
					      GNUTLS_OPENPGP_FMT_BASE64,
					      NULL, NULL);
	if (ret != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to Import "
			 "PGP Private Key '%s': (%d) %s",
			 sc->pgp_key_file, ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}
    }

    /* Load the keyring file */
    if (sc->pgp_ring_file) {
	if (load_datum_from_file(spool, sc->pgp_ring_file, &data) != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Error Reading " "Keyring File '%s'",
			 sc->pgp_ring_file);
	    ret = -1;
	    goto cleanup;
	}

	ret = gnutls_openpgp_keyring_init(&sc->pgp_list);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to initialize"
			 "keyring: (%d) %s", ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}

	ret = gnutls_openpgp_keyring_import(sc->pgp_list, &data,
					   GNUTLS_OPENPGP_FMT_BASE64);
	if (ret < 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			 "GnuTLS: Failed to load "
			 "Keyring File '%s': (%d) %s", sc->pgp_ring_file,
			 ret, gnutls_strerror(ret));
	    ret = -1;
	    goto cleanup;
	}
    }

    if (sc->priorities_str) {
        const char *err;
        ret = gnutls_priority_init(&sc->priorities, sc->priorities_str, &err);

	if (ret < 0) {
	    if (ret == GNUTLS_E_INVALID_REQUEST) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Syntax error parsing priorities string at: %s",
			     err);
	    } else {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: error parsing priorities string");

	    }
	    ret = -1;
	    goto cleanup;
	}
    }

    ret = 0;
  cleanup:
    apr_pool_destroy(spool);

    return ret;
}

int mgs_pkcs11_reinit(server_rec * base_server)
{
    int ret;
    server_rec *s;
    mgs_srvconf_rec *sc;

    gnutls_pkcs11_reinit();

    for (s = base_server; s; s = s->next) {
        sc = (mgs_srvconf_rec *) ap_get_module_config(s->module_config, &gnutls_module);

	    /* gnutls caches the session in a private key, so we need to open
	     * a new one */
	    if (sc->x509_key_file && gnutls_url_is_supported(sc->x509_key_file) != 0) {
	        gnutls_privkey_deinit(sc->privkey_x509);

		ret = gnutls_privkey_init(&sc->privkey_x509);
		if (ret < 0) {
		    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
				 "GnuTLS: Failed to initialize: (%d) %s", ret,
				 gnutls_strerror(ret));
		    goto fail;
		}

		gnutls_privkey_set_pin_function(sc->privkey_x509, pin_callback, sc);

	        ret = gnutls_privkey_import_url(sc->privkey_x509, sc->x509_key_file, 0);
		if (ret < 0) {
		    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
			     "GnuTLS: Failed to Re-Import Private Key URL '%s': (%d) %s",
			     sc->x509_key_file, ret, gnutls_strerror(ret));
		    goto fail;
		}
	    }
    }

    return 0;

 fail:
    gnutls_privkey_deinit(sc->privkey_x509);
    return -1;
}

const char *mgs_set_dh_file(cmd_parms * parms, void *dummy,
			    const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->dh_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_cert_file(cmd_parms * parms, void *dummy,
			      const char *arg)
{

    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->
						 server->module_config,
						 &gnutls_module);

    sc->x509_cert_file = apr_pstrdup(parms->pool, arg);

    return NULL;

}

const char *mgs_set_key_file(cmd_parms * parms, void *dummy,
			     const char *arg)
{

    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->
						 server->module_config,
						 &gnutls_module);

    sc->x509_key_file = apr_pstrdup(parms->pool, arg);

    return NULL;
}

const char *mgs_set_pgpcert_file(cmd_parms * parms, void *dummy,
				 const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->pgp_cert_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_pgpkey_file(cmd_parms * parms, void *dummy,
				const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->pgp_key_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_tickets(cmd_parms * parms, void *dummy,
			    const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->tickets = 0;
    if (strcasecmp("on", arg) == 0) {
	sc->tickets = 1;
    }

    return NULL;
}


#ifdef ENABLE_SRP

const char *mgs_set_srp_tpasswd_file(cmd_parms * parms, void *dummy,
				     const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->srp_tpasswd_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_srp_tpasswd_conf_file(cmd_parms * parms, void *dummy,
					  const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->srp_tpasswd_conf_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

#endif

const char *mgs_set_cache(cmd_parms * parms, void *dummy,
			  const char *type, const char *arg)
{
    const char *err;
    mgs_srvconf_rec *sc =
	ap_get_module_config(parms->server->module_config,
			     &gnutls_module);
    if ((err = ap_check_cmd_context(parms, GLOBAL_ONLY))) {
	return err;
    }

    if (strcasecmp("none", type) == 0) {
	sc->cache_type = mgs_cache_none;
	sc->cache_config = NULL;
	return NULL;
    } else if (strcasecmp("dbm", type) == 0) {
	sc->cache_type = mgs_cache_dbm;
    } else if (strcasecmp("gdbm", type) == 0) {
	sc->cache_type = mgs_cache_gdbm;
    }
#if HAVE_APR_MEMCACHE
    else if (strcasecmp("memcache", type) == 0) {
	sc->cache_type = mgs_cache_memcache;
    }
#endif
    else {
	return "Invalid Type for GnuTLSCache!";
    }

    if (arg == NULL)
	return "Invalid argument 2 for GnuTLSCache!";

    if (sc->cache_type == mgs_cache_dbm
	|| sc->cache_type == mgs_cache_gdbm) {
	sc->cache_config = ap_server_root_relative(parms->pool, arg);
    } else {
	sc->cache_config = apr_pstrdup(parms->pool, arg);
    }

    return NULL;
}

const char *mgs_set_cache_timeout(cmd_parms * parms, void *dummy,
				  const char *arg)
{
    int argint;
    const char *err;
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    if ((err = ap_check_cmd_context(parms, GLOBAL_ONLY))) {
	return err;
    }

    argint = atoi(arg);

    if (argint < 0) {
	return "GnuTLSCacheTimeout: Invalid argument";
    } else if (argint == 0) {
	sc->cache_timeout = 0;
    } else {
	sc->cache_timeout = apr_time_from_sec(argint);
    }

    return NULL;
}

const char *mgs_set_client_verify_method(cmd_parms * parms, void *dummy,
					 const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->
						 server->module_config,
						 &gnutls_module);

    if (strcasecmp("cartel", arg) == 0) {
	sc->client_verify_method = mgs_cvm_cartel;
    } else if (strcasecmp("msva", arg) == 0) {
#ifdef ENABLE_MSVA
	sc->client_verify_method = mgs_cvm_msva;
#else
	return "GnuTLSClientVerifyMethod: msva is not supported";
#endif
    } else {
	return "GnuTLSClientVerifyMethod: Invalid argument";
    }

    return NULL;
}

const char *mgs_set_client_verify(cmd_parms * parms, void *dummy,
				  const char *arg)
{
    int mode;

    if (strcasecmp("none", arg) == 0 || strcasecmp("ignore", arg) == 0) {
	mode = GNUTLS_CERT_IGNORE;
    } else if (strcasecmp("optional", arg) == 0
	       || strcasecmp("request", arg) == 0) {
	mode = GNUTLS_CERT_REQUEST;
    } else if (strcasecmp("require", arg) == 0) {
	mode = GNUTLS_CERT_REQUIRE;
    } else {
	return "GnuTLSClientVerify: Invalid argument";
    }

    /* This was set from a directory context */
    if (parms->path) {
	mgs_dirconf_rec *dc = (mgs_dirconf_rec *) dummy;
	dc->client_verify_mode = mode;
    } else {
	mgs_srvconf_rec *sc = (mgs_srvconf_rec *)
	    ap_get_module_config(parms->server->module_config,
				 &gnutls_module);
	sc->client_verify_mode = mode;
    }

    return NULL;
}

const char *mgs_set_client_ca_file(cmd_parms * parms, void *dummy,
				   const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->x509_ca_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_keyring_file(cmd_parms * parms, void *dummy,
				 const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);

    sc->pgp_ring_file = ap_server_root_relative(parms->pool, arg);

    return NULL;
}

const char *mgs_set_proxy_engine(cmd_parms * parms, void *dummy,
				 const char *arg)
{

    mgs_srvconf_rec *sc = (mgs_srvconf_rec *)
	ap_get_module_config(parms->server->module_config, &gnutls_module);

    if (!strcasecmp(arg, "On")) {
	sc->proxy_enabled = GNUTLS_ENABLED_TRUE;
    } else if (!strcasecmp(arg, "Off")) {
	sc->proxy_enabled = GNUTLS_ENABLED_FALSE;
    } else {
	return "SSLProxyEngine must be set to 'On' or 'Off'";
    }

    return NULL;
}

const char *mgs_set_enabled(cmd_parms * parms, void *dummy,
			    const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->server->
						 module_config,
						 &gnutls_module);
    if (!strcasecmp(arg, "On")) {
	sc->enabled = GNUTLS_ENABLED_TRUE;
    } else if (!strcasecmp(arg, "Off")) {
	sc->enabled = GNUTLS_ENABLED_FALSE;
    } else {
	return "GnuTLSEnable must be set to 'On' or 'Off'";
    }

    return NULL;
}

const char *mgs_set_export_certificates_size(cmd_parms * parms,
					     void *dummy, const char *arg)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(parms->
						 server->module_config,
						 &gnutls_module);
    if (!strcasecmp(arg, "On")) {
	sc->export_certificates_size = 16 * 1024;
    } else if (!strcasecmp(arg, "Off")) {
	sc->export_certificates_size = 0;
    } else {
	char *endptr;
	sc->export_certificates_size = strtol(arg, &endptr, 10);
	while (apr_isspace(*endptr))
	    endptr++;
	if (*endptr == '\0' || *endptr == 'b' || *endptr == 'B') {
	    ;
	} else if (*endptr == 'k' || *endptr == 'K') {
	    sc->export_certificates_size *= 1024;
	} else {
	    return
		"GnuTLSExportCertificates must be set to a size (in bytes) or 'On' or 'Off'";
	}
    }

    return NULL;
}

const char *mgs_set_priorities(cmd_parms * parms, void *dummy,
			       const char *arg)
{

    mgs_srvconf_rec *sc = (mgs_srvconf_rec *)
	ap_get_module_config(parms->server->module_config, &gnutls_module);

    sc->priorities_str = apr_pstrdup(parms->pool, arg);

    return NULL;
}

const char *mgs_set_pin(cmd_parms * parms, void *dummy, const char *arg)
{

    mgs_srvconf_rec *sc = (mgs_srvconf_rec *)
	ap_get_module_config(parms->server->module_config, &gnutls_module);

    sc->pin = apr_pstrdup(parms->pool, arg);

    return NULL;
}

const char *mgs_set_srk_pin(cmd_parms * parms, void *dummy, const char *arg)
{

    mgs_srvconf_rec *sc = (mgs_srvconf_rec *)
	ap_get_module_config(parms->server->module_config, &gnutls_module);

    sc->srk_pin = apr_pstrdup(parms->pool, arg);

    return NULL;
}

static mgs_srvconf_rec *_mgs_config_server_create(apr_pool_t * p,
						  char **err)
{
    mgs_srvconf_rec *sc = apr_pcalloc(p, sizeof(*sc));

    sc->enabled = GNUTLS_ENABLED_UNSET;


    sc->privkey_x509 = NULL;
    sc->privkey_pgp = NULL;
    sc->certs_x509_chain_num = 0;
    sc->cache_timeout = -1;	/* -1 means "unset" */
    sc->cache_type = mgs_cache_unset;
    sc->cache_config = NULL;
    sc->tickets = GNUTLS_ENABLED_UNSET;
    sc->priorities = NULL;
    sc->dh_params = NULL;
    sc->proxy_enabled = GNUTLS_ENABLED_UNSET;
    sc->export_certificates_size = -1;
    sc->client_verify_method = mgs_cvm_unset;

/* this relies on GnuTLS never changing the gnutls_certificate_request_t enum to define -1 */
    sc->client_verify_mode = -1;

    return sc;
}

void *mgs_config_server_create(apr_pool_t * p, server_rec * s)
{
    char *err = NULL;
    mgs_srvconf_rec *sc = _mgs_config_server_create(p, &err);
    if (sc)
	return sc;
    else
	return err;
}

#define gnutls_srvconf_merge(t, unset) sc->t = (add->t == unset) ? base->t : add->t
#define gnutls_srvconf_assign(t) sc->t = add->t

void *mgs_config_server_merge(apr_pool_t * p, void *BASE, void *ADD)
{
    int i;
    char *err = NULL;
    mgs_srvconf_rec *base = (mgs_srvconf_rec *) BASE;
    mgs_srvconf_rec *add = (mgs_srvconf_rec *) ADD;
    mgs_srvconf_rec *sc = _mgs_config_server_create(p, &err);
    if (NULL == sc)
	return err;

    gnutls_srvconf_merge(enabled, GNUTLS_ENABLED_UNSET);
    gnutls_srvconf_merge(tickets, GNUTLS_ENABLED_UNSET);
    gnutls_srvconf_merge(proxy_enabled, GNUTLS_ENABLED_UNSET);
    gnutls_srvconf_merge(export_certificates_size, -1);
    gnutls_srvconf_merge(client_verify_method, mgs_cvm_unset);
    gnutls_srvconf_merge(client_verify_mode, -1);
    gnutls_srvconf_merge(srp_tpasswd_file, NULL);
    gnutls_srvconf_merge(srp_tpasswd_conf_file, NULL);
    gnutls_srvconf_merge(x509_cert_file, NULL);

    gnutls_srvconf_merge(x509_key_file, NULL);
    gnutls_srvconf_merge(x509_ca_file, NULL);
    gnutls_srvconf_merge(pin, NULL);
    gnutls_srvconf_merge(pgp_cert_file, NULL);
    gnutls_srvconf_merge(pgp_key_file, NULL);
    gnutls_srvconf_merge(pgp_ring_file, NULL);
    gnutls_srvconf_merge(dh_file, NULL);
    gnutls_srvconf_merge(priorities_str, NULL);

    /* FIXME: the following items are pre-allocated, and should be
     * properly disposed of before assigning in order to avoid leaks;
     * so at the moment, we can't actually have them in the config.
     * what happens during de-allocation? */
    gnutls_srvconf_assign(ca_list);
    gnutls_srvconf_assign(ca_list_size);
    gnutls_srvconf_assign(cert_pgp);
    gnutls_srvconf_assign(cert_crt_pgp);
    gnutls_srvconf_assign(pgp_list);
    gnutls_srvconf_assign(certs);
    gnutls_srvconf_assign(anon_creds);
    gnutls_srvconf_assign(srp_creds);
    gnutls_srvconf_assign(certs_x509_chain);
    gnutls_srvconf_assign(certs_x509_crt_chain);
    gnutls_srvconf_assign(certs_x509_chain_num);

    /* how do these get transferred cleanly before the data from ADD
     * goes away? */
    gnutls_srvconf_assign(cert_cn);
    for (i = 0; i < MAX_CERT_SAN; i++)
	gnutls_srvconf_assign(cert_san[i]);

    return sc;
}

#undef gnutls_srvconf_merge
#undef gnutls_srvconf_assign

void *mgs_config_dir_merge(apr_pool_t * p, void *basev, void *addv)
{
    mgs_dirconf_rec *new;
    /*    mgs_dirconf_rec *base = (mgs_dirconf_rec *) basev; */
    mgs_dirconf_rec *add = (mgs_dirconf_rec *) addv;

    new = (mgs_dirconf_rec *) apr_pcalloc(p, sizeof(mgs_dirconf_rec));
    new->client_verify_mode = add->client_verify_mode;
    return new;
}

void *mgs_config_dir_create(apr_pool_t * p, char *dir)
{
    mgs_dirconf_rec *dc = apr_palloc(p, sizeof(*dc));
    dc->client_verify_mode = -1;
    return dc;
}
