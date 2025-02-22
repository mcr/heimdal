/*
 * Copyright (c) 1997 - 2010 Kungliga Tekniska Högskolan
 * (Royal Institute of Technology, Stockholm, Sweden).
 * All rights reserved.
 *
 * Portions Copyright (c) 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#undef KRB5_DEPRECATED_FUNCTION
#define KRB5_DEPRECATED_FUNCTION(x)

#include "krb5_locl.h"
#include <assert.h>
#include <com_err.h>

static void _krb5_init_ets(krb5_context);

#define INIT_FIELD(C, T, E, D, F)					\
    (C)->E = krb5_config_get_ ## T ## _default ((C), NULL, (D), 	\
						"libdefaults", F, NULL)

#define INIT_FLAG(C, O, V, D, F)					\
    do {								\
	if (krb5_config_get_bool_default((C), NULL, (D),"libdefaults", F, NULL)) { \
	    (C)->O |= V;						\
        }								\
    } while(0)

static krb5_error_code
copy_enctypes(krb5_context context,
	      const krb5_enctype *in,
	      krb5_enctype **out);

/*
 * Set the list of etypes `ret_etypes' from the configuration variable
 * `name'
 */

static krb5_error_code
set_etypes (krb5_context context,
	    const char *name,
	    krb5_enctype **ret_enctypes)
{
    char **etypes_str;
    krb5_enctype *etypes = NULL;

    etypes_str = krb5_config_get_strings(context, NULL, "libdefaults",
					 name, NULL);
    if(etypes_str){
	int i, j, k;
	for(i = 0; etypes_str[i]; i++);
	etypes = malloc((i+1) * sizeof(*etypes));
	if (etypes == NULL) {
	    krb5_config_free_strings (etypes_str);
	    return krb5_enomem(context);
	}
	for(j = 0, k = 0; j < i; j++) {
	    krb5_enctype e;
	    if(krb5_string_to_enctype(context, etypes_str[j], &e) != 0)
		continue;
	    if (krb5_enctype_valid(context, e) != 0)
		continue;
	    etypes[k++] = e;
	}
	etypes[k] = ETYPE_NULL;
	krb5_config_free_strings(etypes_str);
    }
    *ret_enctypes = etypes;
    return 0;
}

/*
 * read variables from the configuration file and set in `context'
 */

static krb5_error_code
init_context_from_config_file(krb5_context context)
{
    krb5_error_code ret;
    const char * tmp;
    char **s;
    krb5_enctype *tmptypes = NULL;

    INIT_FIELD(context, time, max_skew, 5 * 60, "clockskew");
    INIT_FIELD(context, time, kdc_timeout, 30, "kdc_timeout");
    INIT_FIELD(context, time, host_timeout, 3, "host_timeout");
    INIT_FIELD(context, int, max_retries, 3, "max_retries");

    INIT_FIELD(context, string, http_proxy, NULL, "http_proxy");

    ret = krb5_config_get_bool_default(context, NULL, FALSE,
				       "libdefaults",
				       "allow_weak_crypto", NULL);
    if (ret) {
	krb5_enctype_enable(context, ETYPE_DES_CBC_CRC);
	krb5_enctype_enable(context, ETYPE_DES_CBC_MD4);
	krb5_enctype_enable(context, ETYPE_DES_CBC_MD5);
	krb5_enctype_enable(context, ETYPE_DES_CBC_NONE);
	krb5_enctype_enable(context, ETYPE_DES_CFB64_NONE);
	krb5_enctype_enable(context, ETYPE_DES_PCBC_NONE);
    }

    ret = set_etypes (context, "default_etypes", &tmptypes);
    if(ret)
	return ret;
    free(context->etypes);
    context->etypes = tmptypes;

    /* The etypes member may change during the lifetime
     * of the context. To be able to reset it to
     * config value, we keep another copy.
     */
    free(context->cfg_etypes);
    context->cfg_etypes = NULL;
    if (tmptypes) {
	ret = copy_enctypes(context, tmptypes, &context->cfg_etypes);
	if (ret)
	    return ret;
    }

    ret = set_etypes (context, "default_etypes_des", &tmptypes);
    if(ret)
	return ret;
    free(context->etypes_des);
    context->etypes_des = tmptypes;

    ret = set_etypes (context, "default_as_etypes", &tmptypes);
    if(ret)
	return ret;
    free(context->as_etypes);
    context->as_etypes = tmptypes;

    ret = set_etypes (context, "default_tgs_etypes", &tmptypes);
    if(ret)
	return ret;
    free(context->tgs_etypes);
    context->tgs_etypes = tmptypes;

    ret = set_etypes (context, "permitted_enctypes", &tmptypes);
    if(ret)
	return ret;
    free(context->permitted_enctypes);
    context->permitted_enctypes = tmptypes;

    INIT_FIELD(context, string, default_keytab,
	       KEYTAB_DEFAULT, "default_keytab_name");

    INIT_FIELD(context, string, default_keytab_modify,
	       NULL, "default_keytab_modify_name");

    INIT_FIELD(context, string, time_fmt,
	       "%Y-%m-%dT%H:%M:%S", "time_format");

    INIT_FIELD(context, string, date_fmt,
	       "%Y-%m-%d", "date_format");

    INIT_FIELD(context, bool, log_utc,
	       FALSE, "log_utc");

    context->no_ticket_store =
        getenv("KRB5_NO_TICKET_STORE") != NULL;

    /* init dns-proxy slime */
    tmp = krb5_config_get_string(context, NULL, "libdefaults",
				 "dns_proxy", NULL);
    if(tmp)
	roken_gethostby_setup(context->http_proxy, tmp);
    krb5_free_host_realm (context, context->default_realms);
    context->default_realms = NULL;

    {
	krb5_addresses addresses;
	char **adr, **a;

	krb5_set_extra_addresses(context, NULL);
	adr = krb5_config_get_strings(context, NULL,
				      "libdefaults",
				      "extra_addresses",
				      NULL);
	memset(&addresses, 0, sizeof(addresses));
	for(a = adr; a && *a; a++) {
	    ret = krb5_parse_address(context, *a, &addresses);
	    if (ret == 0) {
		krb5_add_extra_addresses(context, &addresses);
		krb5_free_addresses(context, &addresses);
	    }
	}
	krb5_config_free_strings(adr);

	krb5_set_ignore_addresses(context, NULL);
	adr = krb5_config_get_strings(context, NULL,
				      "libdefaults",
				      "ignore_addresses",
				      NULL);
	memset(&addresses, 0, sizeof(addresses));
	for(a = adr; a && *a; a++) {
	    ret = krb5_parse_address(context, *a, &addresses);
	    if (ret == 0) {
		krb5_add_ignore_addresses(context, &addresses);
		krb5_free_addresses(context, &addresses);
	    }
	}
	krb5_config_free_strings(adr);
    }

    INIT_FIELD(context, bool, scan_interfaces, TRUE, "scan_interfaces");
    INIT_FIELD(context, int, fcache_vno, 0, "fcache_version");
    /* prefer dns_lookup_kdc over srv_lookup. */
    INIT_FIELD(context, bool, srv_lookup, TRUE, "srv_lookup");
    INIT_FIELD(context, bool, srv_lookup, context->srv_lookup, "dns_lookup_kdc");
    INIT_FIELD(context, int, large_msg_size, 1400, "large_message_size");
    INIT_FIELD(context, int, max_msg_size, 1000 * 1024, "maximum_message_size");
    INIT_FLAG(context, flags, KRB5_CTX_F_DNS_CANONICALIZE_HOSTNAME, TRUE, "dns_canonicalize_hostname");
    INIT_FLAG(context, flags, KRB5_CTX_F_CHECK_PAC, TRUE, "check_pac");
    INIT_FLAG(context, flags, KRB5_CTX_F_ENFORCE_OK_AS_DELEGATE, FALSE, "enforce_ok_as_delegate");
    INIT_FLAG(context, flags, KRB5_CTX_F_REPORT_CANONICAL_CLIENT_NAME, FALSE, "report_canonical_client_name");

    /* report_canonical_client_name implies check_pac */
    if (context->flags & KRB5_CTX_F_REPORT_CANONICAL_CLIENT_NAME)
	context->flags |= KRB5_CTX_F_CHECK_PAC;

    free(context->default_cc_name);
    context->default_cc_name = NULL;
    context->default_cc_name_set = 0;
    free(context->configured_default_cc_name);
    context->configured_default_cc_name = NULL;

    tmp = secure_getenv("KRB5_TRACE");
    if (tmp)
        heim_add_debug_dest(context->hcontext, "libkrb5", tmp);
    s = krb5_config_get_strings(context, NULL, "logging", "krb5", NULL);
    if (s) {
	char **p;

        for (p = s; *p; p++)
            heim_add_debug_dest(context->hcontext, "libkrb5", *p);
        krb5_config_free_strings(s);
    }

    tmp = krb5_config_get_string(context, NULL, "libdefaults",
				 "check-rd-req-server", NULL);
    if (tmp == NULL)
	tmp = secure_getenv("KRB5_CHECK_RD_REQ_SERVER");
    if(tmp) {
	if (strcasecmp(tmp, "ignore") == 0)
	    context->flags |= KRB5_CTX_F_RD_REQ_IGNORE;
    }
    ret = krb5_config_get_bool_default(context, NULL, TRUE,
				       "libdefaults",
				       "fcache_strict_checking", NULL);
    if (ret)
	context->flags |= KRB5_CTX_F_FCACHE_STRICT_CHECKING;

    return 0;
}

static krb5_error_code
cc_ops_register(krb5_context context)
{
    context->cc_ops = NULL;
    context->num_cc_ops = 0;

#ifndef KCM_IS_API_CACHE
    krb5_cc_register(context, &krb5_acc_ops, TRUE);
#endif
    krb5_cc_register(context, &krb5_fcc_ops, TRUE);
    krb5_cc_register(context, &krb5_dcc_ops, TRUE);
    krb5_cc_register(context, &krb5_mcc_ops, TRUE);
#ifdef HAVE_SCC
    krb5_cc_register(context, &krb5_scc_ops, TRUE);
#endif
#ifdef HAVE_KCM
#ifdef KCM_IS_API_CACHE
    krb5_cc_register(context, &krb5_akcm_ops, TRUE);
#endif
    krb5_cc_register(context, &krb5_kcm_ops, TRUE);
#endif
#if defined(HAVE_KEYUTILS_H)
    krb5_cc_register(context, &krb5_krcc_ops, TRUE);
#endif
    _krb5_load_ccache_plugins(context);
    return 0;
}

static krb5_error_code
cc_ops_copy(krb5_context context, const krb5_context src_context)
{
    const krb5_cc_ops **cc_ops;

    context->cc_ops = NULL;
    context->num_cc_ops = 0;

    if (src_context->num_cc_ops == 0)
	return 0;

    cc_ops = malloc(sizeof(cc_ops[0]) * src_context->num_cc_ops);
    if (cc_ops == NULL) {
	krb5_set_error_message(context, KRB5_CC_NOMEM,
			       N_("malloc: out of memory", ""));
	return KRB5_CC_NOMEM;
    }

    memcpy(rk_UNCONST(cc_ops), src_context->cc_ops,
	   sizeof(cc_ops[0]) * src_context->num_cc_ops);
    context->cc_ops = cc_ops;
    context->num_cc_ops = src_context->num_cc_ops;

    return 0;
}

static krb5_error_code
kt_ops_register(krb5_context context)
{
    context->num_kt_types = 0;
    context->kt_types     = NULL;

    krb5_kt_register (context, &krb5_fkt_ops);
    krb5_kt_register (context, &krb5_wrfkt_ops);
    krb5_kt_register (context, &krb5_javakt_ops);
    krb5_kt_register (context, &krb5_mkt_ops);
#ifndef HEIMDAL_SMALLER
    krb5_kt_register (context, &krb5_akf_ops);
#endif
    krb5_kt_register (context, &krb5_any_ops);
    return 0;
}

static krb5_error_code
kt_ops_copy(krb5_context context, const krb5_context src_context)
{
    context->num_kt_types = 0;
    context->kt_types     = NULL;

    if (src_context->num_kt_types == 0)
	return 0;

    context->kt_types = malloc(sizeof(context->kt_types[0]) * src_context->num_kt_types);
    if (context->kt_types == NULL)
	return krb5_enomem(context);

    context->num_kt_types = src_context->num_kt_types;
    memcpy(context->kt_types, src_context->kt_types,
	   sizeof(context->kt_types[0]) * src_context->num_kt_types);

    return 0;
}

static const char *sysplugin_dirs[] =  {
#ifdef _WIN32
    "$ORIGIN",
#else
    "$ORIGIN/../lib/plugin/krb5",
#endif
#ifdef __APPLE__
    LIBDIR "/plugin/krb5",
#ifdef HEIM_PLUGINS_SEARCH_SYSTEM
    "/Library/KerberosPlugins/KerberosFrameworkPlugins",
    "/System/Library/KerberosPlugins/KerberosFrameworkPlugins",
#endif
#endif
    NULL
};

static void
init_context_once(void *ctx)
{
    krb5_context context = ctx;
    char **dirs;

#ifdef _WIN32
    dirs = rk_UNCONST(sysplugin_dirs);
#else
    dirs = krb5_config_get_strings(context, NULL, "libdefaults",
				   "plugin_dir", NULL);
    if (dirs == NULL)
	dirs = rk_UNCONST(sysplugin_dirs);
#endif

    _krb5_load_plugins(context, "krb5", (const char **)dirs);

    if (dirs != rk_UNCONST(sysplugin_dirs))
	krb5_config_free_strings(dirs);

    bindtextdomain(HEIMDAL_TEXTDOMAIN, HEIMDAL_LOCALEDIR);
}

/**
 * Initializes the context structure and reads the configuration file
 * /etc/krb5.conf. The structure should be freed by calling
 * krb5_free_context() when it is no longer being used.
 *
 * @param context pointer to returned context
 *
 * @return Returns 0 to indicate success.  Otherwise an errno code is
 * returned.  Failure means either that something bad happened during
 * initialization (typically ENOMEM) or that Kerberos should not be
 * used ENXIO. If the function returns HEIM_ERR_RANDOM_OFFLINE, the
 * random source is not available and later Kerberos calls might fail.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_init_context(krb5_context *context)
{
    static heim_base_once_t init_context = HEIM_BASE_ONCE_INIT;
    krb5_context p;
    krb5_error_code ret;
    char **files;
    uint8_t rnd;

    *context = NULL;

    /**
     * krb5_init_context() will get one random byte to make sure our
     * random is alive.  Assumption is that once the non blocking
     * source allows us to pull bytes, its all seeded and allows us to
     * pull more bytes.
     *
     * Most Kerberos users calls krb5_init_context(), so this is
     * useful point where we can do the checking.
     */
    ret = krb5_generate_random(&rnd, sizeof(rnd));
    if (ret)
	return ret;

    p = calloc(1, sizeof(*p));
    if(!p)
	return ENOMEM;

    if ((p->hcontext = heim_context_init()) == NULL) {
        ret = ENOMEM;
        goto out;
    }

    if (!issuid())
        p->flags |= KRB5_CTX_F_HOMEDIR_ACCESS;

    ret = krb5_get_default_config_files(&files);
    if(ret)
	goto out;
    ret = krb5_set_config_files(p, files);
    krb5_free_config_files(files);
    if(ret)
	goto out;

    /* done enough to load plugins */
    heim_base_once_f(&init_context, p, init_context_once);

    /* init error tables */
    _krb5_init_ets(p);
    cc_ops_register(p);
    kt_ops_register(p);

#ifdef PKINIT
    ret = hx509_context_init(&p->hx509ctx);
    if (ret)
	goto out;
#endif
    if (rk_SOCK_INIT())
	p->flags |= KRB5_CTX_F_SOCKETS_INITIALIZED;

out:
    if (ret) {
	krb5_free_context(p);
	p = NULL;
    } else {
        heim_context_set_log_utc(p->hcontext, p->log_utc);
    }
    *context = p;
    return ret;
}

#ifndef HEIMDAL_SMALLER

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_permitted_enctypes(krb5_context context,
			    krb5_enctype **etypes)
{
    return krb5_get_default_in_tkt_etypes(context, KRB5_PDU_NONE, etypes);
}

/*
 *
 */

static krb5_error_code
copy_etypes (krb5_context context,
	     krb5_enctype *enctypes,
	     krb5_enctype **ret_enctypes)
{
    unsigned int i;

    for (i = 0; enctypes[i]; i++)
	;
    i++;

    *ret_enctypes = malloc(sizeof(enctypes[0]) * i);
    if (*ret_enctypes == NULL)
	return krb5_enomem(context);
    memcpy(*ret_enctypes, enctypes, sizeof(enctypes[0]) * i);
    return 0;
}

/**
 * Make a copy for the Kerberos 5 context, the new krb5_context shoud
 * be freed with krb5_free_context().
 *
 * @param context the Kerberos context to copy
 * @param out the copy of the Kerberos, set to NULL error.
 *
 * @return Returns 0 to indicate success.  Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_copy_context(krb5_context context, krb5_context *out)
{
    krb5_error_code ret = 0;
    krb5_context p;

    *out = NULL;

    p = calloc(1, sizeof(*p));
    if (p == NULL)
	return krb5_enomem(context);

    p->cc_ops = NULL;
    p->etypes = NULL;
    p->kt_types = NULL;
    p->cfg_etypes = NULL;
    p->etypes_des = NULL;
    p->default_realms = NULL;
    p->extra_addresses = NULL;
    p->ignore_addresses = NULL;

    if ((p->hcontext = heim_context_init()) == NULL)
        ret = ENOMEM;

    if (ret == 0) {
        heim_context_set_log_utc(p->hcontext, context->log_utc);
        ret = _krb5_config_copy(context, context->cf, &p->cf);
    }
    if (ret == 0)
        ret = init_context_from_config_file(p);
    if (ret == 0 && context->default_cc_name) {
        free(p->default_cc_name);
        if ((p->default_cc_name = strdup(context->default_cc_name)) == NULL)
            ret = ENOMEM;
    }
    if (ret == 0 && context->default_cc_name_env) {
        free(p->default_cc_name_env);
        if ((p->default_cc_name_env =
             strdup(context->default_cc_name_env)) == NULL)
            ret = ENOMEM;
    }
    if (ret == 0 && context->configured_default_cc_name) {
        free(p->configured_default_cc_name);
        if ((p->configured_default_cc_name =
             strdup(context->configured_default_cc_name)) == NULL)
            ret = ENOMEM;
    }

    if (ret == 0 && context->etypes) {
        free(p->etypes);
	ret = copy_etypes(context, context->etypes, &p->etypes);
    }
    if (ret == 0 && context->cfg_etypes) {
        free(p->cfg_etypes);
	ret = copy_etypes(context, context->cfg_etypes, &p->cfg_etypes);
    }
    if (ret == 0 && context->etypes_des) {
        free(p->etypes_des);
	ret = copy_etypes(context, context->etypes_des, &p->etypes_des);
    }

    if (ret == 0 && context->default_realms) {
        krb5_free_host_realm(context, p->default_realms);
	ret = krb5_copy_host_realm(context,
				   context->default_realms, &p->default_realms);
    }

    /* XXX should copy */
    if (ret == 0)
        _krb5_init_ets(p);

    if (ret == 0)
        ret = cc_ops_copy(p, context);
    if (ret == 0)
        ret = kt_ops_copy(p, context);
    if (ret == 0)
        ret = krb5_set_extra_addresses(p, context->extra_addresses);
    if (ret == 0)
        ret = krb5_set_extra_addresses(p, context->ignore_addresses);
    if (ret == 0)
        ret = _krb5_copy_send_to_kdc_func(p, context);

    if (ret == 0)
        *out = p;
    else
        krb5_free_context(p);
    return ret;
}

#endif

/**
 * Frees the krb5_context allocated by krb5_init_context().
 *
 * @param context context to be freed.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_free_context(krb5_context context)
{
    _krb5_free_name_canon_rules(context, context->name_canon_rules);
    free(context->default_cc_name);
    free(context->default_cc_name_env);
    free(context->configured_default_cc_name);
    free(context->etypes);
    free(context->cfg_etypes);
    free(context->etypes_des);
    free(context->permitted_enctypes);
    free(context->tgs_etypes);
    free(context->as_etypes);
    krb5_free_host_realm (context, context->default_realms);
    krb5_config_file_free (context, context->cf);
    free(rk_UNCONST(context->cc_ops));
    free(context->kt_types);
    krb5_clear_error_message(context);
    krb5_set_extra_addresses(context, NULL);
    krb5_set_ignore_addresses(context, NULL);
    krb5_set_send_to_kdc_func(context, NULL, NULL);

#ifdef PKINIT
    hx509_context_free(&context->hx509ctx);
#endif

    if (context->flags & KRB5_CTX_F_SOCKETS_INITIALIZED) {
 	rk_SOCK_EXIT();
    }

    heim_context_free(&context->hcontext);
    memset(context, 0, sizeof(*context));
    free(context);
}

/**
 * Reinit the context from a new set of filenames.
 *
 * @param context context to add configuration too.
 * @param filenames array of filenames, end of list is indicated with a NULL filename.
 *
 * @return Returns 0 to indicate success.  Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_config_files(krb5_context context, char **filenames)
{
    krb5_error_code ret;
    heim_config_binding *tmp = NULL;

    if ((ret = heim_set_config_files(context->hcontext, filenames,
                                     &tmp)))
        return ret;
    krb5_config_file_free(context, context->cf);
    context->cf = (krb5_config_binding *)tmp;
    return init_context_from_config_file(context);
}

#ifndef HEIMDAL_SMALLER
/**
 * Reinit the context from configuration file contents in a C string.
 * This should only be used in tests.
 *
 * @param context context to add configuration too.
 * @param config configuration.
 *
 * @return Returns 0 to indicate success.  Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_config(krb5_context context, const char *config)
{
    krb5_error_code ret;
    krb5_config_binding *tmp = NULL;

    if ((ret = krb5_config_parse_string_multi(context, config, &tmp)))
        return ret;
#if 0
    /* with this enabled and if there are no config files, Kerberos is
       considererd disabled */
    if (tmp == NULL)
	return ENXIO;
#endif

    krb5_config_file_free(context, context->cf);
    context->cf = tmp;
    ret = init_context_from_config_file(context);
    return ret;
}
#endif

/*
 *  `pq' isn't free, it's up the the caller
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_prepend_config_files(const char *filelist, char **pq, char ***ret_pp)
{
    return heim_prepend_config_files(filelist, pq, ret_pp);
}

/**
 * Prepend the filename to the global configuration list.
 *
 * @param filelist a filename to add to the default list of filename
 * @param pfilenames return array of filenames, should be freed with krb5_free_config_files().
 *
 * @return Returns 0 to indicate success.  Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_prepend_config_files_default(const char *filelist, char ***pfilenames)
{
    return heim_prepend_config_files_default(filelist, krb5_config_file,
                                             "KRB5_CONFIG", pfilenames);
}

/**
 * Get the global configuration list.
 *
 * @param pfilenames return array of filenames, should be freed with krb5_free_config_files().
 *
 * @return Returns 0 to indicate success.  Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_default_config_files(char ***pfilenames)
{
    if (pfilenames == NULL)
        return EINVAL;
    return heim_get_default_config_files(krb5_config_file, "KRB5_CONFIG",
                                         pfilenames);
}

/**
 * Free a list of configuration files.
 *
 * @param filenames list, terminated with a NULL pointer, to be
 * freed. NULL is an valid argument.
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_free_config_files(char **filenames)
{
    heim_free_config_files(filenames);
}

/**
 * Returns the list of Kerberos encryption types sorted in order of
 * most preferred to least preferred encryption type.  Note that some
 * encryption types might be disabled, so you need to check with
 * krb5_enctype_valid() before using the encryption type.
 *
 * @return list of enctypes, terminated with ETYPE_NULL. Its a static
 * array completed into the Kerberos library so the content doesn't
 * need to be freed.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION const krb5_enctype * KRB5_LIB_CALL
krb5_kerberos_enctypes(krb5_context context)
{
    static const krb5_enctype p[] = {
	ETYPE_AES256_CTS_HMAC_SHA1_96,
	ETYPE_AES128_CTS_HMAC_SHA1_96,
	ETYPE_AES256_CTS_HMAC_SHA384_192,
	ETYPE_AES128_CTS_HMAC_SHA256_128,
	ETYPE_DES3_CBC_SHA1,
	ETYPE_ARCFOUR_HMAC_MD5,
	ETYPE_NULL
    };

    static const krb5_enctype weak[] = {
	ETYPE_AES256_CTS_HMAC_SHA1_96,
	ETYPE_AES128_CTS_HMAC_SHA1_96,
	ETYPE_AES256_CTS_HMAC_SHA384_192,
	ETYPE_AES128_CTS_HMAC_SHA256_128,
	ETYPE_DES3_CBC_SHA1,
	ETYPE_DES3_CBC_MD5,
	ETYPE_ARCFOUR_HMAC_MD5,
	ETYPE_DES_CBC_MD5,
	ETYPE_DES_CBC_MD4,
	ETYPE_DES_CBC_CRC,
	ETYPE_NULL
    };

    /*
     * if the list of enctypes enabled by "allow_weak_crypto"
     * are valid, then return the former default enctype list
     * that contained the weak entries.
     */
    if (krb5_enctype_valid(context, ETYPE_DES_CBC_CRC) == 0 &&
        krb5_enctype_valid(context, ETYPE_DES_CBC_MD4) == 0 &&
        krb5_enctype_valid(context, ETYPE_DES_CBC_MD5) == 0 &&
        krb5_enctype_valid(context, ETYPE_DES_CBC_NONE) == 0 &&
        krb5_enctype_valid(context, ETYPE_DES_CFB64_NONE) == 0 &&
        krb5_enctype_valid(context, ETYPE_DES_PCBC_NONE) == 0)
        return weak;

    return p;
}

/*
 *
 */

static krb5_error_code
copy_enctypes(krb5_context context,
	      const krb5_enctype *in,
	      krb5_enctype **out)
{
    krb5_enctype *p = NULL;
    size_t m, n;

    for (n = 0; in[n]; n++)
	;
    n++;
    ALLOC(p, n);
    if(p == NULL)
	return krb5_enomem(context);
    for (n = 0, m = 0; in[n]; n++) {
	if (krb5_enctype_valid(context, in[n]) != 0)
	    continue;
	p[m++] = in[n];
    }
    p[m] = KRB5_ENCTYPE_NULL;
    if (m == 0) {
	free(p);
	krb5_set_error_message (context, KRB5_PROG_ETYPE_NOSUPP,
				N_("no valid enctype set", ""));
	return KRB5_PROG_ETYPE_NOSUPP;
    }
    *out = p;
    return 0;
}


/*
 * set `etype' to a malloced list of the default enctypes
 */

static krb5_error_code
default_etypes(krb5_context context, krb5_enctype **etype)
{
    const krb5_enctype *p = krb5_kerberos_enctypes(context);
    return copy_enctypes(context, p, etype);
}

/**
 * Set the default encryption types that will be use in communcation
 * with the KDC, clients and servers.
 *
 * @param context Kerberos 5 context.
 * @param etypes Encryption types, array terminated with ETYPE_NULL (0).
 * A value of NULL resets the encryption types to the defaults set in the
 * configuration file.
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_default_in_tkt_etypes(krb5_context context,
			       const krb5_enctype *etypes)
{
    krb5_error_code ret;
    krb5_enctype *p = NULL;

    if(!etypes) {
	etypes = context->cfg_etypes;
    }

    if(etypes) {
	ret = copy_enctypes(context, etypes, &p);
	if (ret)
	    return ret;
    }
    if(context->etypes)
	free(context->etypes);
    context->etypes = p;
    return 0;
}

/**
 * Get the default encryption types that will be use in communcation
 * with the KDC, clients and servers.
 *
 * @param context Kerberos 5 context.
 * @param pdu_type request type (AS, TGS or none)
 * @param etypes Encryption types, array terminated with
 * ETYPE_NULL(0), caller should free array with krb5_xfree():
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_default_in_tkt_etypes(krb5_context context,
			       krb5_pdu pdu_type,
			       krb5_enctype **etypes)
{
    krb5_enctype *enctypes = NULL;
    krb5_error_code ret;
    krb5_enctype *p;

    heim_assert(pdu_type == KRB5_PDU_AS_REQUEST || 
		pdu_type == KRB5_PDU_TGS_REQUEST ||
		pdu_type == KRB5_PDU_NONE, "unexpected pdu type");

    if (pdu_type == KRB5_PDU_AS_REQUEST && context->as_etypes != NULL)
	enctypes = context->as_etypes;
    else if (pdu_type == KRB5_PDU_TGS_REQUEST && context->tgs_etypes != NULL)
	enctypes = context->tgs_etypes;
    else if (context->etypes != NULL)
	enctypes = context->etypes;

    if (enctypes != NULL) {
	ret = copy_enctypes(context, enctypes, &p);
	if (ret)
	    return ret;
    } else {
	ret = default_etypes(context, &p);
	if (ret)
	    return ret;
    }
    *etypes = p;
    return 0;
}

/**
 * Init the built-in ets in the Kerberos library.
 *
 * @param context kerberos context to add the ets too
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_init_ets(krb5_context context)
{
}

static void
_krb5_init_ets(krb5_context context)
{
    heim_add_et_list(context->hcontext, initialize_krb5_error_table_r);
    heim_add_et_list(context->hcontext, initialize_asn1_error_table_r);
    heim_add_et_list(context->hcontext, initialize_heim_error_table_r);

    heim_add_et_list(context->hcontext, initialize_k524_error_table_r);
    heim_add_et_list(context->hcontext, initialize_k5e1_error_table_r);

#ifdef COM_ERR_BINDDOMAIN_krb5
    bindtextdomain(COM_ERR_BINDDOMAIN_krb5, HEIMDAL_LOCALEDIR);
    bindtextdomain(COM_ERR_BINDDOMAIN_asn1, HEIMDAL_LOCALEDIR);
    bindtextdomain(COM_ERR_BINDDOMAIN_heim, HEIMDAL_LOCALEDIR);
    bindtextdomain(COM_ERR_BINDDOMAIN_k524, HEIMDAL_LOCALEDIR);
#endif

#ifdef PKINIT
    heim_add_et_list(context->hcontext, initialize_hx_error_table_r);
#ifdef COM_ERR_BINDDOMAIN_hx
    bindtextdomain(COM_ERR_BINDDOMAIN_hx, HEIMDAL_LOCALEDIR);
#endif
#endif
}

/**
 * Make the kerberos library default to the admin KDC.
 *
 * @param context Kerberos 5 context.
 * @param flag boolean flag to select if the use the admin KDC or not.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_set_use_admin_kdc (krb5_context context, krb5_boolean flag)
{
    context->use_admin_kdc = flag;
}

/**
 * Make the kerberos library default to the admin KDC.
 *
 * @param context Kerberos 5 context.
 *
 * @return boolean flag to telling the context will use admin KDC as the default KDC.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_get_use_admin_kdc (krb5_context context)
{
    return context->use_admin_kdc;
}

/**
 * Add extra address to the address list that the library will add to
 * the client's address list when communicating with the KDC.
 *
 * @param context Kerberos 5 context.
 * @param addresses addreses to add
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_add_extra_addresses(krb5_context context, krb5_addresses *addresses)
{

    if(context->extra_addresses)
	return krb5_append_addresses(context,
				     context->extra_addresses, addresses);
    else
	return krb5_set_extra_addresses(context, addresses);
}

/**
 * Set extra address to the address list that the library will add to
 * the client's address list when communicating with the KDC.
 *
 * @param context Kerberos 5 context.
 * @param addresses addreses to set
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_extra_addresses(krb5_context context, const krb5_addresses *addresses)
{
    if(context->extra_addresses)
	krb5_free_addresses(context, context->extra_addresses);

    if(addresses == NULL) {
	if(context->extra_addresses != NULL) {
	    free(context->extra_addresses);
	    context->extra_addresses = NULL;
	}
	return 0;
    }
    if(context->extra_addresses == NULL) {
	context->extra_addresses = malloc(sizeof(*context->extra_addresses));
	if (context->extra_addresses == NULL)
	    return krb5_enomem(context);
    }
    return krb5_copy_addresses(context, addresses, context->extra_addresses);
}

/**
 * Get extra address to the address list that the library will add to
 * the client's address list when communicating with the KDC.
 *
 * @param context Kerberos 5 context.
 * @param addresses addreses to set
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_extra_addresses(krb5_context context, krb5_addresses *addresses)
{
    if(context->extra_addresses == NULL) {
	memset(addresses, 0, sizeof(*addresses));
	return 0;
    }
    return krb5_copy_addresses(context,context->extra_addresses, addresses);
}

/**
 * Add extra addresses to ignore when fetching addresses from the
 * underlaying operating system.
 *
 * @param context Kerberos 5 context.
 * @param addresses addreses to ignore
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_add_ignore_addresses(krb5_context context, krb5_addresses *addresses)
{

    if(context->ignore_addresses)
	return krb5_append_addresses(context,
				     context->ignore_addresses, addresses);
    else
	return krb5_set_ignore_addresses(context, addresses);
}

/**
 * Set extra addresses to ignore when fetching addresses from the
 * underlaying operating system.
 *
 * @param context Kerberos 5 context.
 * @param addresses addreses to ignore
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_ignore_addresses(krb5_context context, const krb5_addresses *addresses)
{
    if(context->ignore_addresses)
	krb5_free_addresses(context, context->ignore_addresses);
    if(addresses == NULL) {
	if(context->ignore_addresses != NULL) {
	    free(context->ignore_addresses);
	    context->ignore_addresses = NULL;
	}
	return 0;
    }
    if(context->ignore_addresses == NULL) {
	context->ignore_addresses = malloc(sizeof(*context->ignore_addresses));
	if (context->ignore_addresses == NULL)
	    return krb5_enomem(context);
    }
    return krb5_copy_addresses(context, addresses, context->ignore_addresses);
}

/**
 * Get extra addresses to ignore when fetching addresses from the
 * underlaying operating system.
 *
 * @param context Kerberos 5 context.
 * @param addresses list addreses ignored
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_ignore_addresses(krb5_context context, krb5_addresses *addresses)
{
    if(context->ignore_addresses == NULL) {
	memset(addresses, 0, sizeof(*addresses));
	return 0;
    }
    return krb5_copy_addresses(context, context->ignore_addresses, addresses);
}

/**
 * Set version of fcache that the library should use.
 *
 * @param context Kerberos 5 context.
 * @param version version number.
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_fcache_version(krb5_context context, int version)
{
    context->fcache_vno = version;
    return 0;
}

/**
 * Get version of fcache that the library should use.
 *
 * @param context Kerberos 5 context.
 * @param version version number.
 *
 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_fcache_version(krb5_context context, int *version)
{
    *version = context->fcache_vno;
    return 0;
}

/**
 * Runtime check if the Kerberos library was complied with thread support.
 *
 * @return TRUE if the library was compiled with thread support, FALSE if not.
 *
 * @ingroup krb5
 */


KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_is_thread_safe(void)
{
#ifdef ENABLE_PTHREAD_SUPPORT
    return TRUE;
#else
    return FALSE;
#endif
}

/**
 * Set if the library should use DNS to canonicalize hostnames.
 *
 * @param context Kerberos 5 context.
 * @param flag if its dns canonicalizion is used or not.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_set_dns_canonicalize_hostname (krb5_context context, krb5_boolean flag)
{
    if (flag)
	context->flags |= KRB5_CTX_F_DNS_CANONICALIZE_HOSTNAME;
    else
	context->flags &= ~KRB5_CTX_F_DNS_CANONICALIZE_HOSTNAME;
}

/**
 * Get if the library uses DNS to canonicalize hostnames.
 *
 * @param context Kerberos 5 context.
 *
 * @return return non zero if the library uses DNS to canonicalize hostnames.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_get_dns_canonicalize_hostname (krb5_context context)
{
    return (context->flags & KRB5_CTX_F_DNS_CANONICALIZE_HOSTNAME) ? 1 : 0;
}

/**
 * Get current offset in time to the KDC.
 *
 * @param context Kerberos 5 context.
 * @param sec seconds part of offset.
 * @param usec micro seconds part of offset.
 *
 * @return returns zero
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_get_kdc_sec_offset (krb5_context context, int32_t *sec, int32_t *usec)
{
    if (sec)
	*sec = context->kdc_sec_offset;
    if (usec)
	*usec = context->kdc_usec_offset;
    return 0;
}

/**
 * Set current offset in time to the KDC.
 *
 * @param context Kerberos 5 context.
 * @param sec seconds part of offset.
 * @param usec micro seconds part of offset.
 *
 * @return returns zero
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
krb5_set_kdc_sec_offset (krb5_context context, int32_t sec, int32_t usec)
{
    context->kdc_sec_offset = sec;
    if (usec >= 0)
	context->kdc_usec_offset = usec;
    return 0;
}

/**
 * Get max time skew allowed.
 *
 * @param context Kerberos 5 context.
 *
 * @return timeskew in seconds.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION time_t KRB5_LIB_CALL
krb5_get_max_time_skew (krb5_context context)
{
    return context->max_skew;
}

/**
 * Set max time skew allowed.
 *
 * @param context Kerberos 5 context.
 * @param t timeskew in seconds.
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION void KRB5_LIB_CALL
krb5_set_max_time_skew (krb5_context context, time_t t)
{
    context->max_skew = t;
}

/*
 * Init encryption types in len, val with etypes.
 *
 * @param context Kerberos 5 context.
 * @param pdu_type type of pdu
 * @param len output length of val.
 * @param val output array of enctypes.
 * @param etypes etypes to set val and len to, if NULL, use default enctypes.

 * @return Returns 0 to indicate success. Otherwise an kerberos et
 * error code is returned, see krb5_get_error_message().
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_error_code KRB5_LIB_CALL
_krb5_init_etype(krb5_context context,
		 krb5_pdu pdu_type,
		 unsigned *len,
		 krb5_enctype **val,
		 const krb5_enctype *etypes)
{
    krb5_error_code ret;

    if (etypes == NULL)
	ret = krb5_get_default_in_tkt_etypes(context, pdu_type, val);
    else
	ret = copy_enctypes(context, etypes, val);
    if (ret)
	return ret;

    if (len) {
	*len = 0;
	while ((*val)[*len] != KRB5_ENCTYPE_NULL)
	    (*len)++;
    }
    return 0;
}

/*
 * Allow homedir access
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
_krb5_homedir_access(krb5_context context)
{
    if (context)
        return !!(context->flags & KRB5_CTX_F_HOMEDIR_ACCESS);
    return !issuid();
}

/**
 * Enable and disable home directory access on either the global state
 * or the krb5_context state. By calling krb5_set_home_dir_access()
 * with context set to NULL, the global state is configured otherwise
 * the state for the krb5_context is modified.
 *
 * For home directory access to be allowed, both the global state and
 * the krb5_context state have to be allowed.
 *
 * @param context a Kerberos 5 context or NULL
 * @param allow allow if TRUE home directory
 * @return the old value
 *
 * @ingroup krb5
 */

KRB5_LIB_FUNCTION krb5_boolean KRB5_LIB_CALL
krb5_set_home_dir_access(krb5_context context, krb5_boolean allow)
{
    krb5_boolean old = _krb5_homedir_access(context);

    if (context) {
	if (allow)
	    context->flags |= KRB5_CTX_F_HOMEDIR_ACCESS;
	else
	    context->flags &= ~KRB5_CTX_F_HOMEDIR_ACCESS;
        heim_context_set_homedir_access(context->hcontext, allow ? 1 : 0);
    }

    return old;
}

