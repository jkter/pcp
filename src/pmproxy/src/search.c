/*
 * Copyright (c) 2020 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include "server.h"
#include <assert.h>

typedef enum pmSearchRestKey {
    RESTKEY_TEXT	= 1,
    RESTKEY_INFO,
} pmSearchRestKey;

typedef struct pmSearchRestCommand {
    const char		*name;
    unsigned int	namelen : 16;
    unsigned int	options : 16;
    pmSearchRestKey	key;
} pmSearchRestCommand;

typedef struct pmSearchBaton {
    struct client	*client;
    pmSearchRestKey	restkey;
    pmSearchTextRequest	request;
    unsigned int	options;
    unsigned int	results;
    sds			suffix;
    sds			clientid;
} pmSearchBaton;

static pmSearchRestCommand commands[] = {
    { .key = RESTKEY_TEXT, .options = HTTP_OPTIONS_GET,
	    .name = "text", .namelen = sizeof("text")-1 },
    { .key = RESTKEY_INFO, .options = HTTP_OPTIONS_GET,
	    .name = "info", .namelen = sizeof("info")-1 },
    { .name = NULL }	/* sentinel */
};

/* constant string keys (initialized during servlet setup) */
static sds PARAM_CLIENT, PARAM_QUERY, PARAM_RETURN, PARAM_HIGHLIGHT;
static sds PARAM_FIELDS, PARAM_LIMIT, PARAM_OFFSET, PARAM_TEXT;

/* constant global strings (read-only) */
static const char pmsearch_success[] = "{\"success\":true}\r\n";
static const char pmsearch_failure[] = "{\"success\":false}\r\n";

static pmSearchRestCommand *
pmsearch_lookup_rest_command(sds url)
{
    pmSearchRestCommand	*cp;
    const char		*name;

    if (sdslen(url) >= (sizeof("/search/") - 1) &&
	strncmp(url, "/search/", sizeof("/search/") - 1) == 0) {
	name = (const char *)url + sizeof("/search/") - 1;
	for (cp = &commands[0]; cp->name; cp++) {
	    if (strncmp(cp->name, name, cp->namelen) == 0)
		return cp;
	}
    }
    return NULL;
}

static void
pmsearch_data_release(struct client *client)
{
    pmSearchBaton	*baton = (pmSearchBaton *)client->u.http.data;

    if (pmDebugOptions.http)
	fprintf(stderr, "%s: %p for client %p\n", "pmsearch_data_release",
			baton, client);

    sdsfree(baton->suffix);
    sdsfree(baton->clientid);
    sdsfree(baton->request.query);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

/*
 * If any request is accompanied by 'client', the client is using
 * this to identify responses.  Wrap the usual response using the
 * identifier - by adding a JSON object at the top level with two
 * fields, 'client' (ID) and 'result' (the rest of the response).
 */
static sds
push_client_identifier(pmSearchBaton *baton, sds result)
{
    if (baton->clientid) {
	baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
	return sdscatfmt(result, "{\"client\":%S,\"result\":", baton->clientid);
    }
    return result;
}

static void
on_pmsearch_metrics(pmSearchMetrics *metrics, void *arg)
{
    pmSearchBaton	*baton = (pmSearchBaton *)arg;
    struct client	*client = baton->client;
    sds			result = http_get_buffer(baton->client);

    result = push_client_identifier(baton, result);
    baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
    result = sdscatprintf(result,
			"{\"docs\":%llu,\"terms\":%llu,\"records\":%llu,"
			"\"records_per_doc_avg\":%.2f,"
			"\"bytes_per_record_avg\":%.2f,"
			"\"inverted_sz_mb\":%.2f,"
			"\"inverted_cap_mb\":%.2f,"
			"\"inverted_cap_ovh\":%.2f,"
			"\"skip_index_size_mb\":%.2f,"
			"\"score_index_size_mb\":%.2f,"
			"\"offsets_per_term_avg\":%.2f,"
			"\"offset_bits_per_record_avg\":%.2f",
		metrics->docs, metrics->terms, metrics->records,
		metrics->records_per_doc_avg, metrics->bytes_per_record_avg,
		metrics->inverted_sz_mb, metrics->inverted_cap_mb,
		metrics->inverted_cap_ovh, metrics->skip_index_size_mb,
		metrics->score_index_size_mb, metrics->offsets_per_term_avg,
		metrics->offset_bits_per_record_avg);

    http_set_buffer(client, result, HTTP_FLAG_JSON);
    http_transfer(client);
}

static void
on_pmsearch_text_result(pmSearchTextResult *search, void *arg)
{
    pmSearchBaton	*baton = (pmSearchBaton *)arg;
    struct client	*client = baton->client;
    const char		*prefix;
    char		buffer[64];
    sds			result = http_get_buffer(baton->client);
    sds			oneline, helptext;

    if (baton->results++ == 0) {
	result = push_client_identifier(baton, result);
	/* once-off header containing metrics - timing, total hits */
	baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_OBJECT);
	pmsprintf(buffer, sizeof(buffer), "%.6f", search->timer);
	result = sdscatfmt(result, "{\"total\":%u,\"elapsed\":%s,\"results\":",
				    search->total, buffer);
	baton->suffix = json_push_suffix(baton->suffix, JSON_FLAG_ARRAY);
	prefix = "[";
    } else {
	prefix = ",";
    }

    oneline = search->oneline;
    oneline = sdscatrepr(sdsempty(), oneline, sdslen(oneline));
    helptext = search->helptext;
    helptext = sdscatrepr(sdsempty(), helptext, sdslen(helptext));

    pmsprintf(buffer, sizeof(buffer), "%.6f", search->score);
    result = sdscatfmt(result,
		    "%s{\"docid\":\"%S\",\"count\":%u,\"score\":%s,"
		    "\"name\":\"%S\",\"type\":\"%s\"," "\"indom\":\"%S\","
		    "\"oneline\":%S,\"helptext\":%S}",
		    prefix, search->docid, baton->results, buffer, search->name,
		    pmSearchTextTypeStr(search->type), search->indom,
		    oneline, helptext);

    sdsfree(helptext);
    sdsfree(oneline);

    http_set_buffer(client, result, HTTP_FLAG_JSON);
    http_transfer(client);
}

static void
on_pmsearch_done(int status, void *arg)
{
    pmSearchBaton	*baton = (pmSearchBaton *)arg;
    struct client	*client = baton->client;
    http_options	options = baton->options;
    http_flags		flags = client->u.http.flags;
    http_code		code;
    sds			msg;

    if (status == 0) {
	code = HTTP_STATUS_OK;
	/* complete current response with JSON suffix if needed */
	if ((msg = baton->suffix) == NULL) {	/* empty OK response */
	    if (baton->clientid)
		msg = sdscatfmt(sdsempty(),
				"{\"client\":%S,\"success\":%s}\r\n",
				baton->clientid, "true");
	    else
		msg = sdsnewlen(pmsearch_success, sizeof(pmsearch_success) - 1);
	}
	baton->suffix = NULL;
    } else {
	if (((code = client->u.http.parser.status_code)) == 0)
	    code = HTTP_STATUS_BAD_REQUEST;
	if (baton->clientid)
	    msg = sdscatfmt(sdsempty(),
				"{\"client\":%S,\"success\":%s}\r\n",
				baton->clientid, "false");
	else
	    msg = sdsnewlen(pmsearch_failure, sizeof(pmsearch_failure) - 1);
	flags |= HTTP_FLAG_JSON;
    }
    http_reply(client, msg, code, flags, options);
}

static void
pmsearch_setup(void *arg)
{
    if (pmDebugOptions.search)
	fprintf(stderr, "search module setup (arg=%p)\n", arg);
}

static void
pmsearch_log(pmLogLevel level, sds message, void *arg)
{
    pmSearchBaton	*baton = (pmSearchBaton *)arg;

    proxylog(level, message, baton->client->proxy);
}

static pmSearchSettings pmsearch_settings = {
    .callbacks.on_text_result	= on_pmsearch_text_result,
    .callbacks.on_metrics	= on_pmsearch_metrics,
    .callbacks.on_done		= on_pmsearch_done,
    .module.on_setup		= pmsearch_setup,
    .module.on_info		= pmsearch_log,
};

static void
pmsearch_setup_request_parameters(struct client *client,
		pmSearchBaton *baton, dict *parameters)
{
    dictEntry		*entry;
    sds			*values, value;
    int			i, nvalues = 0;

    if (parameters) {
	/* allow all APIs to pass(-through) a 'client' parameter */
	if ((entry = dictFind(parameters, PARAM_CLIENT)) != NULL) {
	    value = dictGetVal(entry);   /* leave sds value, dup'd below */
	    baton->clientid = sdscatrepr(sdsempty(), value, sdslen(value));
	}
    }

    /* default to querying most */
    baton->request.infields_name = 1;
    baton->request.infields_indom = 0;
    baton->request.infields_oneline = 1;
    baton->request.infields_helptext = 1;

    /* default to returning all */
    baton->request.return_name = 1;
    baton->request.return_indom = 1;
    baton->request.return_oneline = 1;
    baton->request.return_helptext = 1;

    switch (baton->restkey) {
    case RESTKEY_TEXT:
	/* expect a search query string */
	if (parameters == NULL) {
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	    break;
	} else if ((entry = dictFind(parameters, PARAM_QUERY)) != NULL) {
	    baton->request.query = dictGetVal(entry);   /* get sds value */
	    dictSetVal(parameters, entry, NULL);   /* claim this */
	} else {
	    client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	    break;
	}
	/* optional parameters - flags, result count and pagination offset */
	baton->request.flags = 0;
	if ((entry = dictFind(parameters, PARAM_HIGHLIGHT))) {
	    if ((value = dictGetVal(entry)) == NULL) {	/* all */
		baton->request.highlight_name = 1;
		baton->request.highlight_indom = 1;
		baton->request.highlight_oneline = 1;
		baton->request.highlight_helptext = 1;
	    } else {
		values = sdssplitlen(value, sdslen(value), ",", 1, &nvalues);
		for (i = 0; values && i < nvalues; i++) {
		    if (strcmp(values[i], "name") == 0)
			baton->request.highlight_name = 1;
		    else if (strcmp(values[i], "indom") == 0)
			baton->request.highlight_indom = 1;
		    else if (strcmp(values[i], "oneline") == 0)
			baton->request.highlight_oneline = 1;
		    else if (strcmp(values[i], "helptext") == 0)
			baton->request.highlight_helptext = 1;
		}
		sdsfreesplitres(values, nvalues);
	    }
	}
	if ((entry = dictFind(parameters, PARAM_RETURN))) {
	    baton->request.return_name = 0;
	    baton->request.return_indom = 0;
	    baton->request.return_oneline = 0;
	    baton->request.return_helptext = 0;
	    if ((value = dictGetVal(entry)) != NULL) {	/* no text */
		values = sdssplitlen(value, sdslen(value), ",", 1, &nvalues);
		for (i = 0; values && i < nvalues; i++) {
		    if (strcmp(values[i], "name") == 0)
			baton->request.return_name = 1;
		    else if (strcmp(values[i], "indom") == 0)
			baton->request.return_indom = 1;
		    else if (strcmp(values[i], "oneline") == 0)
			baton->request.return_oneline = 1;
		    else if (strcmp(values[i], "helptext") == 0)
			baton->request.return_helptext = 1;
		}
		sdsfreesplitres(values, nvalues);
	    }
	}
	if ((entry = dictFind(parameters, PARAM_FIELDS))) {
	    baton->request.infields_name = 0;
	    baton->request.infields_indom = 0;
	    baton->request.infields_oneline = 0;
	    baton->request.infields_helptext = 0;
	    if ((value = dictGetVal(entry)) != NULL) {
		values = sdssplitlen(value, sdslen(value), ",", 1, &nvalues);
		for (i = 0; values && i < nvalues; i++) {
		    if (strcmp(values[i], "name") == 0)
			baton->request.highlight_name = 1;
		    else if (strcmp(values[i], "indom") == 0)
			baton->request.highlight_indom = 1;
		    else if (strcmp(values[i], "oneline") == 0)
			baton->request.highlight_oneline = 1;
		    else if (strcmp(values[i], "helptext") == 0)
			baton->request.highlight_helptext = 1;
		}
		sdsfreesplitres(values, nvalues);
	    }
	}
	if ((value = (sds)dictFetchValue(parameters, PARAM_LIMIT)))
	    baton->request.count = strtoul(value, NULL, 0);
	if ((value = (sds)dictFetchValue(parameters, PARAM_OFFSET)))
	    baton->request.offset = strtoul(value, NULL, 0);
	break;

    case RESTKEY_INFO:
	break;

    default:
	client->u.http.parser.status_code = HTTP_STATUS_BAD_REQUEST;
	break;
    }
}

/*
 * Test if this is a pmsearch REST API command, and if so which one.
 * If this servlet is handling this URL, ensure space for state exists
 * and indicate acceptance for processing this URL via the return code.
 */
static int
pmsearch_request_url(struct client *client, sds url, dict *parameters)
{
    pmSearchBaton	*baton;
    pmSearchRestCommand	*command;

    if ((command = pmsearch_lookup_rest_command(url)) == NULL)
	return 0;

    if ((baton = calloc(1, sizeof(*baton))) != NULL) {
	client->u.http.data = baton;
	baton->client = client;
	baton->restkey = command->key;
	baton->options = command->options;
	pmsearch_setup_request_parameters(client, baton, parameters);
    } else {
	client->u.http.parser.status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    return 1;
}

static int
pmsearch_request_headers(struct client *client, struct dict *headers)
{
    if (pmDebugOptions.http)
	fprintf(stderr, "series servlet headers (client=%p)\n", client);
    return 0;
}

static int
pmsearch_request_body(struct client *client, const char *content, size_t length)
{
    if (pmDebugOptions.http)
	fprintf(stderr, "series servlet body (client=%p)\n", client);
    return 0;
}

static int
pmsearch_request_done(struct client *client)
{
    pmSearchBaton	*baton = (pmSearchBaton *)client->u.http.data;
    int			sts;

    if (client->u.http.parser.status_code) {
	on_pmsearch_done(-EINVAL, baton);
	return 1;
    }

    if (client->u.http.parser.method == HTTP_OPTIONS ||
	client->u.http.parser.method == HTTP_TRACE ||
	client->u.http.parser.method == HTTP_HEAD) {
	on_pmsearch_done(0, baton);
	return 0;
    }

    switch (baton->restkey) {
    case RESTKEY_TEXT:
	if ((sts = pmSearchTextQuery(&pmsearch_settings, &baton->request, baton)) < 0)
	    on_pmsearch_done(sts, baton);
	break;

    case RESTKEY_INFO:
	if ((sts = pmSearchInfo(&pmsearch_settings, PARAM_TEXT, baton)) < 0)
	    on_pmsearch_done(sts, baton);
	break;

    default:
	on_pmsearch_done(-EINVAL, baton);
	break;
    }
    return 0;
}

static void
pmsearch_servlet_setup(struct proxy *proxy)
{
    mmv_registry_t	*metric_registry = proxymetrics(proxy, METRICS_SEARCH);

    PARAM_CLIENT = sdsnew("clientid");
    PARAM_TEXT = sdsnew("text");
    PARAM_QUERY = sdsnew("query");
    PARAM_FIELDS = sdsnew("fields");
    PARAM_RETURN = sdsnew("return");
    PARAM_HIGHLIGHT = sdsnew("highlight");
    PARAM_LIMIT = sdsnew("limit");
    PARAM_OFFSET = sdsnew("offset");

    pmSearchSetSlots(&pmsearch_settings.module, proxy->slots);
    pmSearchSetEventLoop(&pmsearch_settings.module, proxy->events);
    pmSearchSetConfiguration(&pmsearch_settings.module, proxy->config);
    pmSearchSetMetricRegistry(&pmsearch_settings.module, metric_registry);

    pmSearchSetup(&pmsearch_settings.module, proxy);
}

static void
pmsearch_servlet_close(struct proxy *proxy)
{
    pmSearchClose(&pmsearch_settings.module);
    proxymetrics_close(proxy, METRICS_SEARCH);

    sdsfree(PARAM_CLIENT);
    sdsfree(PARAM_TEXT);
    sdsfree(PARAM_QUERY);
    sdsfree(PARAM_FIELDS);
    sdsfree(PARAM_RETURN);
    sdsfree(PARAM_HIGHLIGHT);
    sdsfree(PARAM_LIMIT);
    sdsfree(PARAM_OFFSET);
}

struct servlet pmsearch_servlet = {
    .name		= "search",
    .setup 		= pmsearch_servlet_setup,
    .close 		= pmsearch_servlet_close,
    .on_url		= pmsearch_request_url,
    .on_headers		= pmsearch_request_headers,
    .on_body		= pmsearch_request_body,
    .on_done		= pmsearch_request_done,
    .on_release		= pmsearch_data_release,
};