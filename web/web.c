/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2014-2016 John Seamons, ZL/KF6VO

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "kiwi.h"
#include "types.h"
#include "config.h"
#include "misc.h"
#include "timer.h"
#include "web.h"
#include "net.h"
#include "coroutines.h"
#include "mongoose.h"
#include "nbuf.h"
#include "cfg.h"
#include "str.h"
#include "ext_int.h"

// This file is compiled twice into two different object files:
// Once with EDATA_EMBED defined when installed as the production server in /usr/local/bin
// Once with EDATA_DEVEL defined when compiled into the build directory during development 

user_iface_t user_iface[] = {
	KIWI_UI_LIST
	{0}
};

user_iface_t *find_ui(int port)
{
	user_iface_t *ui = user_iface;
	while (ui->port) {
		if (ui->port == port)
			return ui;
		ui++;
	}
	return NULL;
}

void webserver_connection_cleanup(conn_t *c)
{
	nbuf_cleanup(&c->c2s);
	nbuf_cleanup(&c->s2c);
}


extern const char *edata_embed(const char *, size_t *);
extern const char *edata_always(const char *, size_t *);
u4_t mtime_obj_keep_edata_always_o;

static const char* edata(const char *uri, bool cache_check, size_t *size, u4_t *mtime, char **free_buf)
{
	const char* data = NULL;
	bool absPath = (uri[0] == '/');
	
#ifdef EDATA_EMBED
	// The normal background daemon loads files from in-memory embedded data for speed.
	// In development mode these files are always loaded from the local filesystem.
	data = edata_embed(uri, size);
	if (data) {
		// In production mode the only thing we have is the server binary build time.
		// But this is okay since because that's the origin of the data and the binary is
		// only updated when a software update occurs.
		*mtime = timer_server_build_unix_time();
		web_printf("EDATA           edata_always server build: mtime=%lu/%lx %s\n", *mtime, *mtime, uri);
	}
#endif

	// some large, seldom-changed files are always loaded from memory, even in development mode
	if (!data) {
		data = edata_always(uri, size);
		if (data) {
#ifdef EDATA_EMBED
			// In production mode the only thing we have is the server binary build time.
			// But this is okay since because that's the origin of the data and the binary is
			// only updated when a software update occurs.
			*mtime = timer_server_build_unix_time();
			web_printf("EDATA           edata_always server build: mtime=%lu/%lx %s\n", *mtime, *mtime, uri);
#else
			// In development mode this is better than the constantly-changing server binary
			// (i.e. the obj_keep/edata_always.o file is rarely updated).
			// NB: mtime_obj_keep_edata_always_o is only updated once per server restart.
			*mtime = mtime_obj_keep_edata_always_o;
			web_printf("EDATA           edata_always.o: mtime=%lu/%lx %s\n", *mtime, *mtime, uri);
#endif
		}
	}

#ifdef EDATA_EMBED
	// only root-referenced files are opened from filesystem when in embedded (production) mode
	if (!absPath)
		return data;
#endif

	// to speed edit-copy-compile-debug development, load the files from the local filesystem
	bool free_uri2 = false;
	char *uri2 = (char *) uri;

#ifdef EDATA_DEVEL
	if (!absPath) {
		asprintf(&uri2, "web/%s", uri);
		free_uri2 = true;
	}
#endif

	// try as a local file
	// NB: in embedded mode this can be true if loading an extension from an absolute path,
	// so this code is not included in an "#ifdef EDATA_DEVEL".
	if (!data) {
		struct stat st;
		if (cache_check) {
			// don't read the file yet -- just return stats for caching determination
			if (stat(uri2, &st) == 0) {
				*size = st.st_size;
				*mtime = st.st_mtime;
				web_printf("EDATA           cache check file: mtime=%lu/%lx %s\n", *mtime, *mtime, uri2);
				data = (char *) "non-null";
				// don't set *free_buf
			}
		} else {
			int fd = open(uri2, O_RDONLY);
			if (fd >= 0) {
				struct stat st;
				fstat(fd, &st);
				*size = st.st_size;
				*mtime = st.st_mtime;
				web_printf("EDATA           fetch file: mtime=%lu/%lx %s\n", *mtime, *mtime, uri2);
				data = (char *) kiwi_malloc("req-file", *size);
				*free_buf = (char *) data;
				ssize_t rsize = read(fd, (void *) data, *size);
				assert(rsize == *size);
				close(fd);
			}
		}
	}

	if (free_uri2) free(uri2);
	return data;
}

struct iparams_t {
	char *id, *val;
};

#define	N_IPARAMS	256
static iparams_t iparams[N_IPARAMS];
static int n_iparams;

void iparams_add(const char *id, char *val)
{
	iparams_t *ip = &iparams[n_iparams];
	asprintf(&ip->id, "%s", (char *) id);
	asprintf(&ip->val, "%s", val);
	ip++; n_iparams++;
}

void index_params_cb(cfg_t *cfg, void *param, jsmntok_t *jt, int seq, int hit, int lvl, int rem)
{
	char *json = cfg_get_json(NULL);
	if (json == NULL || jt->type != JSMN_STRING)
		return;
	
	check(n_iparams < N_IPARAMS);
	iparams_t *ip = &iparams[n_iparams];
	char *s = &json[jt->start];
	int n = jt->end - jt->start;
	if (JSMN_IS_ID(jt)) {
		ip->id = (char *) malloc(n + SPACE_FOR_NULL);
		mg_url_decode(s, n, (char *) ip->id, n + SPACE_FOR_NULL, 0);
		//printf("index_params_cb: %d %d/%d/%d/%d ID %d <%s>\n", n_iparams, seq, hit, lvl, rem, n, ip->id);
	} else {
		ip->val = (char *) malloc(n + SPACE_FOR_NULL);
		// Leave it encoded in case it's a string. [not done currently because although this fixes
		// substitution in .js files it breaks inline substitution for HTML files]
		// Non-string types shouldn't have any escaped characters.
		//strncpy(ip->val, s, n); ip->val[n] = '\0';
		mg_url_decode(s, n, (char *) ip->val, n + SPACE_FOR_NULL, 0);
		//printf("index_params_cb: %d %d/%d/%d/%d %s: %s\n", n_iparams, seq, hit, lvl, rem, ip->id, ip->val);
		n_iparams++;
	}
}

void reload_index_params()
{
	int i;
	
	//printf("reload_index_params: free %d\n", n_iparams);
	for (i=0; i < n_iparams; i++) {
		free(iparams[i].id);
		free(iparams[i].val);
	}
	n_iparams = 0;
	//cfg_walk("index_html_params", cfg_print_tok, NULL);
	cfg_walk("index_html_params", index_params_cb, NULL);
	
	
	// add the list of extensions
	// FIXME move this outside of the repeated calls to reload_index_params
	iparams_t *ip = &iparams[n_iparams];
	asprintf(&ip->id, "EXT_LIST_JS");
	char *s = extint_list_js();
	asprintf(&ip->val, "%s", kstr_sp(s));
	kstr_free(s);
	//printf("EXT_LIST_JS: %d %s", n_iparams, ip->val);
	ip++; n_iparams++;

	char *cs = (char *) cfg_string("owner_info", NULL, CFG_REQUIRED);
	iparams_add("OWNER_INFO", cs);
	cfg_string_free(cs);
}


/*
	Architecture of web server:
		c2s = client-to-server
		s2c = server-to-client
	
	NB: The only "push" s2c data is server websocket output (stream data and messages).
	Other s2c data are responses to c2s requests.
	
	Called by Kiwi server code:
		// server polled check of websocket SET messages
		web_to_app()
			return nbuf_dequeue(c2s nbuf)
		
		// server demand push of websocket stream data
		app_to_web(buf)
			buf => nbuf_allocq(s2c)

		// server demand push of websocket message data (no need to use nbufs)
		send_msg*()
			mg_websocket_write()
	
	Called by (or on behalf of) mongoose web server:
		// event requests _from_ web server:
		// (prompted by data coming into web server)
		mg_create_server()
			ev_handler()
				MG_REQUEST:
					request()
						is_websocket:
							copy mc data to nbufs:
								mc data => nbuf_allocq(c2s) [=> web_to_app()]
						file:
							return file/AJAX data to server:
								mg_send_header
								(file data, AJAX) => mg_send_data()
				MG_CLOSE:
					rx_server_websocket(WS_MODE_CLOSE)
				MG_AUTH:
					MG_TRUE
		
		// polled push of data _to_ web server
		TASK:
		web_server()
			mg_poll_server()	// also forces mongoose internal buffering to write to sockets
			mg_iterate_over_connections()
				iterate_callback()
					is_websocket:
						[app_to_web() =>] nbuf_dequeue(s2c) => mg_websocket_write()
					other:
						ERROR
			LOOP
					
*/


// c2s
// client to server
// 1) websocket: SET messages sent from .js via (ws).send(), received via websocket connection threads
//		no response returned (unless s2c send_msg*() done)
// 2) HTTP GET: normal browser file downloads, response returned (e.g. .html, .css, .js, images files)
// 3) HTTP GET: AJAX requests, response returned (e.g. "GET /status")
//		eliminating most of these in favor of websocket messages so connection auth can be performed
// 4) HTTP PUT: e.g. kiwi_ajax_send() upload photo file, response returned

int web_to_app(conn_t *c, nbuf_t **nbp)
{
	nbuf_t *nb;
	
	if (c->stop_data) return 0;
	nb = nbuf_dequeue(&c->c2s);
	if (!nb) {
		*nbp = NULL;
		return 0;
	}
	assert(!nb->done && !nb->expecting_done && nb->buf && nb->len);
	nb->expecting_done = TRUE;
	*nbp = nb;
	
	return nb->len;
}

void web_to_app_done(conn_t *c, nbuf_t *nb)
{
	assert(nb->expecting_done && !nb->done);
	nb->expecting_done = FALSE;
	nb->done = TRUE;
}


// s2c
// server to client
// 1) websocket: {AUD, FFT} data streams received by .js via (ws).onmessage()
// 2) websocket: {MSG, ADM, MFG, EXT, DAT} messages sent by send_msg*(), received via open_websocket() msg_cb/recv_cb routines
// 3) 

void app_to_web(conn_t *c, char *s, int sl)
{
	if (c->stop_data) return;
	nbuf_allocq(&c->s2c, s, sl);
	//NextTask("s2c");
}


// event requests _from_ web server:
// (prompted by data coming into web server)
//	1) handle incoming websocket data
//	2) HTML GET ordinary requests, including cache info requests
//	3) HTML GET AJAX requests
//	4) HTML PUT requests

static int request(struct mg_connection *mc, enum mg_event ev) {
	int i;
	size_t edata_size = 0;
	const char *edata_data;
	char *free_buf = NULL;

	if (mc->is_websocket) {
		// This handler is called for each incoming websocket frame, one or more
		// times for connection lifetime.
		char *s = mc->content;
		int sl = mc->content_len;
		//printf("WEBSOCKET: len %d uri <%s>\n", sl, mc->uri);
		if (sl == 0) {
			//printf("----KA %d\n", mc->remote_port);
			return MG_TRUE;	// keepalive?
		}
		
		conn_t *c = rx_server_websocket(mc, WS_MODE_ALLOC);
		if (c == NULL) {
			s[sl]=0;
			//if (!down) lprintf("rx_server_websocket(alloc): msg was %d <%s>\n", sl, s);
			return MG_FALSE;
		}
		if (c->stop_data) return MG_FALSE;
		
		nbuf_allocq(&c->c2s, s, sl);
		
		if (mc->content_len == 4 && !memcmp(mc->content, "exit", 4)) {
			//printf("----EXIT %d\n", mc->remote_port);
			return MG_FALSE;
		}
		
		return MG_TRUE;
	} else
	
	if (ev == MG_CACHE_RESULT) {
		web_printf("MG_CACHE_RESULT %s:%05d%s cached=%s (etag_match=%d || not_mod_since=%d) mtime=%lu/%lx",
			mc->remote_ip, mc->remote_port, (strcmp(mc->remote_ip, "::ffff:152.66.211.30") == 0)? "[sdr.hu]":"",
			mc->cache_info.cached? "YES":"NO", mc->cache_info.etag_match, mc->cache_info.not_mod_since,
			mc->cache_info.st.st_mtime, mc->cache_info.st.st_mtime);

		if (!mc->cache_info.if_mod_since) {
			float diff = ((float) time_diff_s(mc->cache_info.st.st_mtime, mc->cache_info.client_mtime)) / 60.0;
			char suffix = 'm';
			if (diff >= 60.0 || diff <= -60.0) {
				diff /= 60.0;
				suffix = 'h';
				if (diff >= 24.0 || diff <= -24.0) {
					diff /= 24.0;
					suffix = 'd';
				}
			}
			web_printf("[%+.1f%c]", diff, suffix);
		}
		
		web_printf(" %s\n", mc->uri);
		return MG_TRUE;
	} else {
		web_printf("----\n");
		
		if (strcmp(mc->uri, "/") == 0)
			strcpy((char *) mc->uri, "index.html");
		else
		if (mc->uri[0] == '/') mc->uri++;
		
		// SECURITY: prevent escape out of local directory
		mg_remove_double_dots_and_double_slashes((char *) mc->uri);

		char *ouri = (char *) mc->uri;
		char *uri;
		bool free_uri = FALSE, has_prefix = FALSE, is_extension = FALSE;
		u4_t mtime = 0;
		
		//printf("URL <%s>\n", ouri);
		char *suffix = strrchr(ouri, '.');

		if (suffix && (strcmp(suffix, ".json") == 0 || strcmp(suffix, ".json/") == 0)) {
			lprintf("attempt to fetch config file: %s query=<%s> from %s\n", ouri, mc->query_string, mc->remote_ip);
			return MG_FALSE;
		}
		
		// if uri uses a subdir we know about just use the absolute path
		if (strncmp(ouri, "kiwi/", 5) == 0) {
			uri = ouri;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "extensions/", 11) == 0) {
			uri = ouri;
			has_prefix = TRUE;
			is_extension = TRUE;
		} else
		if (strncmp(ouri, "pkgs/", 5) == 0) {
			uri = ouri;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "config/", 7) == 0) {
			asprintf(&uri, "%s/%s", DIR_CFG, &ouri[7]);
			free_uri = TRUE;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "kiwi.config/", 12) == 0) {
			asprintf(&uri, "%s/%s", DIR_CFG, &ouri[12]);
			free_uri = TRUE;
			has_prefix = TRUE;
		} else {
			// use name of active ui as subdir
			user_iface_t *ui = find_ui(mc->local_port);
			// should never not find match since we only listen to ports in ui table
			assert(ui);
			asprintf(&uri, "%s/%s", ui->name, ouri);
			free_uri = TRUE;
		}
		//printf("---- HTTP: uri %s (%s)\n", ouri, uri);

		// try as file from in-memory embedded data or local filesystem
		edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
		
		// try again with ".html" appended
		if (!edata_data) {
			char *uri2;
			asprintf(&uri2, "%s.html", uri);
			if (free_uri) free(uri);
			uri = uri2;
			free_uri = TRUE;
			edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
		}
		
		// try looking in "kiwi" subdir as a default
		if (!edata_data && !has_prefix) {
			if (free_uri) free(uri);
			asprintf(&uri, "kiwi/%s", ouri);
			free_uri = TRUE;

			// try as file from in-memory embedded data or local filesystem
			edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
			
			// try again with ".html" appended
			if (!edata_data) {
				if (free_uri) free(uri);
				asprintf(&uri, "kiwi/%s.html", ouri);
				free_uri = TRUE;
				edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
			}
		}

		// For extensions, try looking in external extension directory (outside this package).
		// SECURITY: But ONLY for extensions! Don't allow any other root-referenced accesses.
		// "ouri" has been previously protected against "../" directory escape.
		if (!edata_data && is_extension) {
			if (free_uri) free(uri);
			asprintf(&uri, "/root/%s", ouri);
			free_uri = TRUE;

			// try as file from in-memory embedded data or local filesystem
			edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
			
			// try again with ".html" appended
			if (!edata_data) {
				if (free_uri) free(uri);
				asprintf(&uri, "/root/%s.html", ouri);
				free_uri = TRUE;
				edata_data = edata(uri, ev == MG_CACHE_INFO, &edata_size, &mtime, &free_buf);
			}
		}

		// try as AJAX request
		bool isAJAX = false;
		if (!edata_data) {
		
			// don't try AJAX during the MG_CACHE_INFO pass
			if (ev == MG_CACHE_INFO) {
				if (free_uri) free(uri);
				if (free_buf) kiwi_free("req-*", free_buf);
				return MG_FALSE;
			}
			edata_data = rx_server_ajax(mc);	// mc->uri is ouri without ui->name prefix
			if (edata_data) {
				edata_size = kstr_len((char *) edata_data);
				isAJAX = true;
			}
		}

		// give up
		if (!edata_data) {
			printf("unknown URL: %s (%s) query=<%s> from %s\n", ouri, uri, mc->query_string, mc->remote_ip);
			if (free_uri) free(uri);
			if (free_buf) kiwi_free("req-*", free_buf);
			return MG_FALSE;
		}
		
		// for *.html and *.css process %[substitution]
		// fixme: don't just panic because the config params are bad
		bool free_edata = false;
		suffix = strrchr(uri, '.');
		
		if (!isAJAX && ev != MG_CACHE_INFO && suffix && (strcmp(suffix, ".html") == 0 || strcmp(suffix, ".css") == 0)) {
			int nsize = edata_size;
			char *html_buf = (char *) kiwi_malloc("html_buf", nsize);
			free_edata = true;
			char *cp = (char *) edata_data, *np = html_buf, *pp;
			int cl, sl, pl, nl;

			//printf("checking for \"%%[\": %s %d\n", uri, nsize);

			for (cl=nl=0; cl < edata_size;) {
				if (*cp == '%' && *(cp+1) == '[') {
					cp += 2; cl += 2; pp = cp; pl = 0;
					while (*cp != ']' && cl < edata_size) { cp++; cl++; pl++; }
					cp++; cl++;
					
					for (i=0; i < n_iparams; i++) {
						iparams_t *ip = &iparams[i];
						if (strncmp(pp, ip->id, pl) == 0) {
							sl = strlen(ip->val);

							// expand buffer
							html_buf = (char *) kiwi_realloc("html_buf", html_buf, nsize+sl);
							np = html_buf + nl;		// in case buffer moved
							nsize += sl;
							//printf("%d %%[%s] %d <%s>\n", nsize, ip->id, sl, ip->val);
							strcpy(np, ip->val); np += sl; nl += sl;
							break;
						}
					}
					
					if (i == n_iparams) {
						// not found, put back original
						strcpy(np, "%["); np += 2;
						strncpy(np, pp, pl); np += pl;
						*np++ = ']';
						nl += pl + 3;
					}
				} else {
					*np++ = *cp++; nl++; cl++;
				}

				assert(nl <= nsize);
			}
			
			edata_data = html_buf;
			assert((np - html_buf) == nl);
			edata_size = nl;
		}
		
		// Add version checking to each .js file served.
		// Add to end of file so line numbers printed in javascript errors are not effected.
		char *ver = NULL;
		int ver_size = 0;
		bool isJS = (suffix && strcmp(suffix, ".js") == 0);
		if (!isAJAX && isJS) {
			asprintf(&ver, "kiwi_check_js_version.push({ VERSION_MAJ:%d, VERSION_MIN:%d, file:'%s' });\n", VERSION_MAJ, VERSION_MIN, uri);
			ver_size = strlen(ver);
		}

		// Tell web server the file size and modify time so it can make a decision about caching.
		// Modify time _was_ conservative: server start time as .js files have version info appended.
		// Modify time is now:
		//		server running in background (production) mode: build time of server binary.
		//		server running in foreground (development) mode: stat is fetched from filesystems, else build time of server binary.
		
		// NB: Will see cases of etag_match=N but not_mod_since=Y because of %[] substitution.
		// The size in the etag is different due to the substitution, but the underlying file mtime hasn't changed.
		
		// FIXME: Is what we do here re caching really correct? Do we need to be returning "Cache-Control: must-revalidate"?
		
		int rtn = MG_TRUE;
  		mc->cache_info.st.st_size = edata_size + ver_size;
  		if (!isAJAX) assert(mtime != 0);
  		mc->cache_info.st.st_mtime = mtime;
		
		if (!(isAJAX && ev == MG_CACHE_INFO)) {		// don't print for isAJAX + MG_CACHE_INFO nop case
			web_printf("%-15s %s:%05d%s size=%6d mtime=%lu/%lx %s %s\n", (ev == MG_CACHE_INFO)? "MG_CACHE_INFO" : "MG_REQUEST",
				mc->remote_ip, mc->remote_port, (strcmp(mc->remote_ip, "::ffff:152.66.211.30") == 0)? "[sdr.hu]":"",
				mc->cache_info.st.st_size, mtime, mtime, isAJAX? mc->uri : uri, mg_get_mime_type(isAJAX? mc->uri : uri, "text/plain"));
		}

		if (ev == MG_CACHE_INFO) {
			//if (isAJAX)
			// FIXME jksx because of interaction with version checking, never cache .js files
			if (isAJAX || isJS)
				rtn = MG_FALSE;
		} else {
		
			// NB: prevent AJAX responses from getting cached by not sending standard headers which include etag etc!
			if (isAJAX) {
				printf("AJAX: %s %s\n", mc->uri, uri);
				mg_send_header(mc, "Content-Type", "text/plain");
				
				// needed by, e.g., auto-discovery port scanner
				// SECURITY FIXME: can we detect a special request header in the pre-flight and return this selectively?
				
				// An <iframe sandbox="allow-same-origin"> is not sufficient for subsequent
				// non-same-origin XHRs because the
				// "Access-Control-Allow-Origin: *" must be specified in the pre-flight.
				mg_send_header(mc, "Access-Control-Allow-Origin", "*");
			} else
			if (isJS) {
				// FIXME jksx because of interaction with version checking, never cache .js files
				mg_send_header(mc, "Content-Type", mg_get_mime_type(uri, "text/plain"));
			} else {
				mg_send_standard_headers(mc, uri, &mc->cache_info.st, "OK", (char *) "", true);
			}
			
			mg_send_header(mc, "Server", "KiwiSDR/Mongoose");
			mg_send_data(mc, kstr_sp((char *) edata_data), edata_size);

			if (ver != NULL) {
				mg_send_data(mc, ver, ver_size);
			}
		}
		
		if (ver != NULL) free(ver);
		if (free_edata) kiwi_free("html_buf", (void *) edata_data);
		if (isAJAX) kstr_free((char *) edata_data);
		if (free_uri) free(uri);
		if (free_buf) kiwi_free("req-*", free_buf);
		
		if (ev != MG_CACHE_INFO) http_bytes += edata_size;
		return rtn;
	}
}

// event requests _from_ web server:
// (prompted by data coming into web server)
static int ev_handler(struct mg_connection *mc, enum mg_event ev) {
  
	//printf("ev_handler %d:%d len %d\n", mc->local_port, mc->remote_port, (int) mc->content_len);
	//printf("MG_REQUEST: URI:%s query:%s\n", mc->uri, mc->query_string);
	if (ev == MG_REQUEST || ev == MG_CACHE_INFO || ev == MG_CACHE_RESULT) {
		int r = request(mc, ev);
		return r;
	} else
	if (ev == MG_CLOSE) {
		//printf("MG_CLOSE\n");
		rx_server_websocket(mc, WS_MODE_CLOSE);
		mc->connection_param = NULL;
		return MG_TRUE;
	} else
	if (ev == MG_AUTH) {
		//printf("MG_AUTH\n");
		return MG_TRUE;
	} else {
		//printf("MG_OTHER\n");
		return MG_FALSE;
	}
}

// polled send of data _to_ web server
static int iterate_callback(struct mg_connection *mc, enum mg_event ev)
{
	int ret;
	nbuf_t *nb;
	
	if (ev == MG_POLL && mc->is_websocket) {
		conn_t *c = rx_server_websocket(mc, WS_MODE_LOOKUP);
		if (c == NULL)  return MG_FALSE;

		while (TRUE) {
			if (c->stop_data) break;
			nb = nbuf_dequeue(&c->s2c);
			//printf("s2c CHK port %d nb %p\n", mc->remote_port, nb);
			
			if (nb) {
				assert(!nb->done && nb->buf && nb->len);

				//#ifdef SND_SEQ_CHECK
				#if 0
				// check timing of audio output
				snd_pkt_t *out = (snd_pkt_t *) nb->buf;
				if (c->type == STREAM_SOUND && strncmp(out->h.id, "AUD ", 4) == 0) {
					u4_t now = timer_ms();
					if (!c->audio_check) {
						c->audio_epoch = now;
						c->audio_sequence = c->audio_pkts_sent = c->audio_last_time = c->sum2 = 0;
						c->audio_check = true;
					}
					double audio_rate = ext_get_sample_rateHz();
					u4_t expected1 = c->audio_epoch + (u4_t)((1.0/audio_rate * (512*4) * c->audio_pkts_sent)*1000.0);
					s4_t diff1 = (s4_t)(now - expected1);
					u4_t expected2 = (u4_t)((1.0/audio_rate * (512*4))*1000.0);
					s4_t diff2 = c->audio_last_time? (s4_t)((now - c->audio_last_time) - expected2) : 0;
					c->audio_last_time = now;
					#define DIFF1 30
					#define DIFF2 1
					if (diff1 < -DIFF1 || diff1 > DIFF1 || diff2 < -DIFF2 || diff2 > DIFF2) {
						printf("SND%d %4d Q%d d1=%6.3f d2=%6.3f/%6.3f %.6f %f\n",
							c->rx_channel, c->audio_sequence, nbuf_queued(&c->s2c)+1,
							(float)diff1/1e4, (float)diff2/1e4, (float)c->sum2/1e4,
							adc_clock/1e6, audio_rate);
					}
					c->sum2 += diff2;
					if (out->h.seq != c->audio_sequence) {
						printf("SND%d SEQ expecting %d got %d, %s -------------------------\n",
							c->rx_channel, c->audio_sequence, out->h.seq, c->user);
						c->audio_sequence = out->h.seq;
					}
					c->audio_sequence++;
					c->audio_pkts_sent++;
				}
				#endif

				//printf("s2c %d WEBSOCKET: %d %p\n", mc->remote_port, nb->len, nb->buf);
				ret = mg_websocket_write(mc, WS_OPCODE_BINARY, nb->buf, nb->len);
				if (ret<=0) printf("$$$$$$$$ socket write ret %d\n", ret);
				nb->done = TRUE;
			} else {
				break;
			}
		}
	} else {
		if (ev != MG_POLL) printf("$$$$$$$$ s2c %d OTHER: %d len %d\n", mc->remote_port, (int) ev, (int) mc->content_len);
	}
	
	//NextTask("web callback");
	
	return MG_TRUE;
}

void web_server(void *param)
{
	user_iface_t *ui = (user_iface_t *) param;
	struct mg_server *server = ui->server;
	const char *err;
	
	while (1) {
		mg_poll_server(server, 0);		// passing 0 effects a poll
		mg_iterate_over_connections(server, iterate_callback);
		TaskSleepUsec(WEB_SERVER_POLL_US);
	}
}

void web_server_init(ws_init_t type)
{
	int i;
	user_iface_t *ui = user_iface;
	static bool init;
	
	if (!init) {
		nbuf_init();

		// add the new "port_ext" config param if needed
		// done here because web_server_init(WS_INIT_CREATE) called earlier than rx_server_init() in main.c
		int port = admcfg_int("port", NULL, CFG_REQUIRED);
		bool error;
		admcfg_int("port_ext", &error, CFG_OPTIONAL);
		if (error) {
			admcfg_set_int("port_ext", port);
			admcfg_save_json(cfg_adm.json);
		}
		
#ifdef EDATA_DEVEL
		struct stat st;
		scall("stat edata_always", stat("./obj_keep/edata_always.o", &st));
		mtime_obj_keep_edata_always_o = st.st_mtime;
#endif

		init = TRUE;
	}
	
	if (type == WS_INIT_CREATE) {
		// if specified, override the default port number
		if (alt_port) {
			ddns.port = ddns.port_ext = alt_port;
		} else {
			ddns.port = admcfg_int("port", NULL, CFG_REQUIRED);
			ddns.port_ext = admcfg_int("port_ext", NULL, CFG_REQUIRED);
		}
		lprintf("listening on %s port %d/%d for \"%s\"\n", alt_port? "alt":"default",
			ddns.port, ddns.port_ext, ui->name);
		ui->port = ddns.port;
		ui->port_ext = ddns.port_ext;
	} else

	if (type == WS_INIT_START) {
		reload_index_params();
		services_start(SVCS_RESTART_FALSE);
	}

	// create webserver port(s)
	for (i = 0; ui->port; i++) {
	
		if (type == WS_INIT_CREATE) {
			// FIXME: stopgap until admin page supports config of multiple UIs
			if (i != 0) {
				ui->port = ddns.port + i;
				ui->port_ext = ddns.port_ext + i;
			}
			
			ui->server = mg_create_server(NULL, ev_handler);
			//mg_set_option(ui->server, "document_root", "./");		// if serving from file system
			char *s_port;
			asprintf(&s_port, "[::]:%d", ui->port);
			if (mg_set_option(ui->server, "listening_port", s_port) != NULL) {
				lprintf("network port %s for \"%s\" in use\n", s_port, ui->name);
				lprintf("app already running in background?\ntry \"make stop\" (or \"m stop\") first\n");
				xit(-1);
			}
			lprintf("webserver for \"%s\" on port %s\n", ui->name, mg_get_option(ui->server, "listening_port"));
			free(s_port);
		} else {	// WS_INIT_START
			CreateTask(web_server, ui, WEBSERVER_PRIORITY);
		}
		
		ui++;
		if (down) break;
	}
}
