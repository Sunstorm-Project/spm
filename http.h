/*
 * http.h - HTTP/HTTPS client for Solaris 7 with native OpenSSL
 *
 * Uses raw BSD sockets for HTTP and OpenSSL (libssl/libcrypto)
 * for HTTPS. Loads package-owned OpenSSL3 from /opt/sst/lib.
 *
 * On Solaris, link with: -lsocket -lnsl -ldl
 */

#ifndef SPM_HTTP_H
#define SPM_HTTP_H

#include <stddef.h>

/* Response from an HTTP request */
typedef struct {
    int   status_code;   /* HTTP status code (200, 404, etc.) */
    char *body;          /* Response body (malloc'd, caller frees) */
    long  body_len;      /* Length of body in bytes */
    char *content_type;  /* Content-Type header value (malloc'd) */
    char *location;      /* Location header for redirects (malloc'd) */
    long  content_length; /* Content-Length from headers, -1 if unknown */
} http_response_t;

/* Progress callback for downloads: (bytes_received, total_bytes) */
typedef void (*http_progress_fn)(long current, long total);

/*
 * Initialize the HTTP subsystem (OpenSSL).
 * Call once at startup.  Returns 0 on success, -1 on error.
 */
int http_init(void);

/*
 * Shut down the HTTP subsystem (free OpenSSL context).
 * Call once at exit.
 */
void http_shutdown(void);

/*
 * Set the path to the CA certificate bundle.
 * If not called, uses /opt/sst/etc/ssl/cert.pem by default.
 */
void http_set_ca_bundle(const char *path);

/*
 * Perform an HTTP/HTTPS GET request.
 * Handles up to 5 redirects.  HTTPS is handled natively via OpenSSL.
 * Returns 0 on success, -1 on error.
 * Caller must free resp with http_response_free().
 */
int http_get(const char *url, http_response_t *resp);

/*
 * Download a URL to a local file (streaming, low memory).
 * Handles redirects.  progress_cb is called periodically (may be NULL).
 * Returns 0 on success, -1 on error.
 */
int http_download(const char *url, const char *dest_path,
                  http_progress_fn progress_cb);

/*
 * Free the contents of an http_response_t (but not the struct itself).
 */
void http_response_free(http_response_t *resp);

#endif /* SPM_HTTP_H */
