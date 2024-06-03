//
// OAuth API implementation for CUPS.
//
// Copyright © 2024 by OpenPrinting.
// Copyright © 2017-2024 by Michael R Sweet
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include "cups-private.h"
#include "oauth.h"
#include "form.h"
#include <sys/stat.h>

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreServices/CoreServices.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
extern char **environ;			// @private@
#endif // __APPLE__


//
// Local types...
//

typedef enum _cups_otype_e
{
  _CUPS_OTYPE_ACCESS,			// Access token
  _CUPS_OTYPE_METADATA,			// Server metadata
  _CUPS_OTYPE_REFRESH			// Refresh token
} _cups_otype_t;


//
// Local functions...
//

static http_t	*oauth_connect(const char *uri, char *host, size_t host_size, int *port, char *resource, size_t resource_size);
static char	*oauth_copy_response(http_t *http);
static char	*oauth_make_path(char *buffer, size_t bufsize, const char *auth_uri, const char *resource_uri, _cups_otype_t otype);


//
// 'cupsOAuthClearTokens()' - Clear any cached authorization or refresh tokens.
//

void
cupsOAuthClearTokens(
    const char *auth_uri,		// I - Authorization server URI
    const char *resource_uri)		// I - Resource server URI
{
  char	filename[1024];			// Token filename


  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_ACCESS))
    unlink(filename);

  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_REFRESH))
    unlink(filename);
}


//
// 'cupsOAuthCopyAccessToken()' - Get a cached access token.
//
// This function makes a copy of a cached access token and any
// associated expiration time for the given authorization and resource
// server combination.  The returned access token must be freed using
// the `free` function.  `NULL` is returned if no token is cached.
//

char *					// O - Access token
cupsOAuthCopyAccessToken(
    const char *auth_uri,		// I - Authorization server FQDN
    const char *resource_uri,		// I - Resource server FQDN
    time_t     *access_expires)		// O - Access expiration time or `NULL` for don't care
{
  char		filename[1024],		// Token filename
		buffer[1024],		// Token buffer
		*bufptr,		// Pointer into token buffer
		*access_token = NULL;	// Access token
  int		fd;			// File descriptor
  ssize_t	bytes;			// Bytes read


  // Range check input...
  if (access_expires)
    *access_expires = 0;
 
  // See if we have a token file...
  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_ACCESS) && (fd = open(filename, O_RDONLY)) >= 0)
  {
    if ((bytes = read(fd, buffer, sizeof(buffer) - 1)) < 0)
      bytes = 0;

    close(fd);

    buffer[bytes] = '\0';
    if ((bufptr = strchr(buffer, '\n')) != NULL)
    {
      *bufptr++ = '\0';

      if (access_expires)
        *access_expires = strtol(bufptr, NULL, 10);
    }

    access_token = strdup(buffer);
  }
 
  return (access_token);
}


//
// 'cupsOAuthCopyMetadata()' - Get the metadata for an authorization server.
//

cups_json_t *				// O - JSON metadata or `NULL` on error
cupsOAuthCopyMetadata(
    const char *auth_uri)		// I - Authorization server URI
{
  char		filename[1024];		// Local metadata filename
  struct stat	fileinfo;		// Local metadata file info
  char		filedate[256],		// Local metadata modification date
		host[256],		// Hostname
		resource[256];		// Resource path
  int		port;			// Port to use
  http_t	*http;			// Connection to server
  http_status_t	status;			// Request status
  size_t	i;			// Looping var
  static const char * const paths[] =	// Metadata paths
  {
    "/.well-known/oauth-authorization-server",
    "/.well-known/openid-configuration"
  };


  // Get existing metadata...
  if (!oauth_make_path(filename, sizeof(filename), auth_uri, /*resource_uri*/NULL, _CUPS_OTYPE_METADATA))
    return (NULL);

  if (stat(filename, &fileinfo))
    memset(&fileinfo, 0, sizeof(fileinfo));

  if (fileinfo.st_mtime)
    httpGetDateString(fileinfo.st_mtime, filedate, sizeof(filedate));
  else
    filedate[0] = '\0';

  // Don't bother connecting if the metadata was updated recently...
  if ((time(NULL) - fileinfo.st_mtime) <= 60)
    goto load_metadata;

  // Try getting the metadata...
  if ((http = oauth_connect(auth_uri, host, sizeof(host), &port, resource, sizeof(resource))) == NULL)
    return (NULL);

  for (i = 0; i < (sizeof(paths) / sizeof(paths[0])); i ++)
  {
    cupsCopyString(resource, paths[i], sizeof(resource));

    do
    {
      if (!_cups_strcasecmp(httpGetField(http, HTTP_FIELD_CONNECTION), "close"))
      {
        httpClearFields(http);
        if (!httpReconnect(http, /*msec*/30000, /*cancel*/NULL))
        {
	  status = HTTP_STATUS_ERROR;
	  break;
        }
      }

      httpClearFields(http);

      httpSetField(http, HTTP_FIELD_IF_MODIFIED_SINCE, filedate);
      if (!httpWriteRequest(http, "GET", resource))
      {
        if (httpReconnect(http, 30000, NULL))
        {
          status = HTTP_STATUS_UNAUTHORIZED;
          continue;
        }
        else
        {
          status = HTTP_STATUS_ERROR;
	  break;
        }
      }

      while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE)
        ;

      if (status >= HTTP_STATUS_MULTIPLE_CHOICES && status <= HTTP_STATUS_SEE_OTHER)
      {
        // Redirect
	char	lscheme[32],		// Location scheme
		luserpass[256],		// Location user:password (not used)
		lhost[256],		// Location hostname
		lresource[256];		// Location resource path
	int	lport;			// Location port

        if (httpSeparateURI(HTTP_URI_CODING_ALL, httpGetField(http, HTTP_FIELD_LOCATION), lscheme, sizeof(lscheme), luserpass, sizeof(luserpass), lhost, sizeof(lhost), &lport, lresource, sizeof(lresource)) < HTTP_URI_STATUS_OK)
	  break;			// Don't redirect to an invalid URI

        if (_cups_strcasecmp(host, lhost) || port != lport)
	  break;			// Don't redirect off this host

        // Redirect to a local resource...
        cupsCopyString(resource, lresource, sizeof(resource));
      }
    }
    while (status >= HTTP_STATUS_MULTIPLE_CHOICES && status <= HTTP_STATUS_SEE_OTHER);

    if (status == HTTP_STATUS_NOT_MODIFIED)
    {
      // Metadata isn't changed, stop now...
      break;
    }
    else if (status == HTTP_STATUS_OK)
    {
      // Copy the metadata to the file...
      int	fd;			// Local metadata file
      char	buffer[8192];		// Copy buffer
      size_t	bytes;			// Bytes read

      if ((fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600)) < 0)
      {
        httpFlush(http);
        break;
      }

      while ((bytes = httpRead(http, buffer, sizeof(buffer))) > 0)
        write(fd, buffer, (size_t)bytes);

      close(fd);
      break;
    }
  }

  if (status != HTTP_STATUS_OK && status != HTTP_STATUS_NOT_MODIFIED)
  {
    // Remove old cached data...
    unlink(filename);
  }

  httpClose(http);

  // Return the cached metadata, if any...
  load_metadata:

  return (cupsJSONImportFile(filename));
}


//
// 'cupsOAuthCopyRefreshToken()' - Get a cached refresh token.
//
// This function makes a copy of a cached refresh token for the given
// authorization and resource server combination.  The returned refresh
// token must be freed using the `free` function.  `NULL` is returned
// if no token is cached.
//

char *					// O - Refresh token
cupsOAuthCopyRefreshToken(
    const char *auth_uri,		// I - Authorization server FQDN
    const char *resource_uri)		// I - Resource server FQDN
{
  char		filename[1024],		// Token filename
		buffer[1024],		// Token buffer
		*bufptr,		// Pointer into token buffer
		*refresh_token = NULL;	// Refresh token
  int		fd;			// File descriptor
  ssize_t	bytes;			// Bytes read


  // See if we have a token file...
  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_REFRESH) && (fd = open(filename, O_RDONLY)) >= 0)
  {
    if ((bytes = read(fd, buffer, sizeof(buffer) - 1)) < 0)
      bytes = 0;

    close(fd);

    buffer[bytes] = '\0';
    if ((bufptr = strchr(buffer, '\n')) != NULL)
      *bufptr++ = '\0';

    refresh_token = strdup(buffer);
  }
 
  return (refresh_token);
}


//
// 'cupsOAuthDoAuthorize()' - Start authorization using a web browser.
//

bool					// O - `true` on success, `false` otherwise
cupsOAuthDoAuthorize(
    cups_json_t *metadata,		// I - Server metadata
    const char  *resource_uri,		// I - Resource URI
    const char  *redirect_uri,		// I - Redirection URI
    const char  *client_id,		// I - `client_id` value
    const char  *state,			// I - `state` value
    const char  *code_verifier,		// I - `code_verifier` value
    const char  *scope)			// I - Space-delimited scopes
{
  const char	*authorization_ep;	// Authorization endpoint
  char		*url;			// URL for authorization page
  bool		status = true;		// Return status
  unsigned char	sha256[32];		// SHA-256 hash of code verifier
  char		code_challenge[64];	// Hashed code verifier string
  size_t	num_vars = 0;		// Number of form variables
  cups_option_t	*vars = NULL;		// Form variables


  // Range check input...
  if (!metadata || (authorization_ep = cupsJSONGetString(cupsJSONFind(metadata, "authorization_endpoint"))) == NULL || !redirect_uri || !client_id)
    return (false);

  // Make the authorization URL using the information supplied...
  num_vars = cupsAddOption("response_type", "code", num_vars, &vars);
  num_vars = cupsAddOption("client_id", client_id, num_vars, &vars);
  num_vars = cupsAddOption("redirect_uri", redirect_uri, num_vars, &vars);

  if (scope)
    num_vars = cupsAddOption("scope", scope, num_vars, &vars);

  if (state)
    num_vars = cupsAddOption("state", state, num_vars, &vars);

  if (code_verifier)
  {
    cupsHashData("sha2-256", code_verifier, strlen(code_verifier), sha256, sizeof(sha256));
    httpEncode64(code_challenge, (int)sizeof(code_challenge), (char *)sha256, (int)sizeof(sha256), true);
    num_vars = cupsAddOption("code_challenge", code_challenge, num_vars, &vars);
  }

  url = cupsFormEncode(authorization_ep, num_vars, vars);

  cupsFreeOptions(num_vars, vars);

#ifdef __APPLE__
  CFURLRef cfurl = CFURLCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)url, (CFIndex)strlen(url), kCFStringEncodingASCII, NULL);

  if (cfurl)
  {
    if (LSOpenCFURLRef(cfurl, NULL) != noErr)
      status = false;			// Couldn't open URL

    CFRelease(cfurl);
  }
  else
  {
    status = false;			// Couldn't create CFURL object
  }

#else
  pid_t		pid = 0;		// Process ID
  int		estatus;		// Exit status
  const char	*xdg_open_argv[3];	// xdg-open arguments


  xdg_open_argv[0] = "xdg-open";
  xdg_open_argv[1] = url;
  xdg_open_argv[2] = NULL;

  if (posix_spawnp(&pid, "xdg-open", NULL, NULL, (char * const *)xdg_open_argv, environ))
    status = false;			// Couldn't run xdg-open
  else if (waitpid(pid, &estatus, 0))
    status = false;			// Couldn't get exit status
  else if (estatus)
    status = false;			// Non-zero exit status
#endif // __APPLE__

  free(url);

  return (status);
}


//
// 'cupsOAuthDoRefresh()' - Refresh an access token.
//

char *					// O - New access token or `NULL` on error
cupsOAuthDoRefresh(
    cups_json_t *metadata,		// I - Authorization server metadata
    const char  *resource_uri,		// I - Resource URI
    const char  *refresh_token,		// I - Refresh token
    time_t      *access_expires)	// O - New access token expiration time
{
  http_t	*http = NULL;		// HTTP connection
  const char	*token_ep;		// token_endpoint
  char		host[256],		// token_endpoint host
		resource[256];		// token_endpoint resource
  int		port;			// token_endpoint port
  http_status_t	status;			// Response status
  size_t	num_form = 0;		// Number of form variables
  cups_option_t	*form = NULL;		// Form variables
  char		*form_data = NULL;	// POST form data
  size_t	form_length;		// Length of data
  char		*json_data = NULL;	// JSON response data
  cups_json_t	*json;			// JSON variables
  char		*access_token = NULL;	// New access token


  // Range check input...
  if (access_expires)
    *access_expires = 0;

  if (!metadata || (token_ep = cupsJSONGetString(cupsJSONFind(metadata, "token_endpoint"))) == NULL || !refresh_token)
    return (NULL);

  // Prepare form data to get an access token...
  num_form = cupsAddOption("grant_type", "refresh_token", num_form, &form);
  num_form = cupsAddOption("refresh_token", refresh_token, num_form, &form);

  if ((form_data = cupsFormEncode(/*url*/NULL, num_form, form)) == NULL)
    goto done;

  form_length = strlen(form_data);

  // Send a POST request with the form data...
  if ((http = oauth_connect(token_ep, host, sizeof(host), &port, resource, sizeof(resource))) == NULL)
    goto done;

  httpClearFields(http);
  httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "application/x-www-form-urlencoded");
  httpSetLength(http, form_length);

  if (!httpWriteRequest(http, "POST", resource))
  {
    if (!httpReconnect(http, 30000, NULL))
      goto done;

    if (!httpWriteRequest(http, "POST", resource))
      goto done;
  }

  if (httpWrite(http, form_data, form_length) < form_length)
    goto done;

  while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);

  if (status == HTTP_STATUS_OK)
  {
    const char	*access_value,		// access_token value
		*refresh_value;		// refresh_token value
    double	expires_in;		// expires_in value
    time_t	access_expvalue;	// Expiration in seconds

    json_data = oauth_copy_response(http);
    json      = cupsJSONImportString(json_data);

    access_value  = cupsJSONGetString(cupsJSONFind(json, "access_token"));
    expires_in    = cupsJSONGetNumber(cupsJSONFind(json, "expires_in"));
    refresh_value = cupsJSONGetString(cupsJSONFind(json, "refresh_token"));

    if (expires_in > 0.0)
      access_expvalue = time(NULL) + (long)expires_in;
    else
      access_expvalue = 0;

    cupsOAuthSetTokens(token_ep, resource_uri, access_value, access_expvalue, refresh_value);

    if (access_value)
      access_token = strdup(access_value);

    if (access_expires)
      *access_expires = access_expvalue;

    cupsJSONDelete(json);
    free(json_data);
  }
  else
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, httpStatusString(status), false);
  }

  // Close the connection and return whatever we got...
  done:

  httpClose(http);

  cupsFreeOptions(num_form, form);
  free(form_data);

  return (access_token);
}


//
// 'cupsOAuthDoRegisterClient()' - Register a client application.
//

char *					// O - `client_id` string or `NULL` on error
cupsOAuthDoRegisterClient(
    cups_json_t *metadata,		// I - Authorization server metadata
    const char  *redirect_uri,		// I - Redirection URL
    const char  *client_name,		// I - Client name or `NULL` for none
    const char  *client_uri,		// I - Client information URL or `NULL` for none
    const char  *software_id,		// I - Client software UUID or `NULL` for none
    const char  *software_version,	// I - Client software version number or `NULL` for none
    const char  *logo_uri,		// I - Logo URL or `NULL` for none
    const char  *tos_uri)		// I - Terms-of-service URL or `NULL` for none
{
  const char	*registration_ep;	// Registration endpoint
  char		*client_id = NULL;	// `client_id` string
  http_t	*http = NULL;		// HTTP connection
  char		host[256],		// Hostname
		resource[256];		// Registration endpoint resource
  int		port;			// Port number
  http_status_t	status;			// Response status
  char		*json_data = NULL;	// JSON data
  size_t	json_length;		// Length of JSON data
  cups_json_t	*json,			// JSON variables
		*jarray;		// JSON array
  const char	*value;			// JSON value


  // Range check input...
  if (!metadata)
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Missing metadata."), true);
    return (NULL);
  }
  else if ((registration_ep = cupsJSONGetString(cupsJSONFind(metadata, "registration_endpoint"))) == NULL)
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Missing registration_endpoint."), true);
    return (NULL);
  }
  else if (!redirect_uri)
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Missing redirect_uri."), true);
    return (NULL);
  }

  // Prepare JSON data to register the client application...
  json = cupsJSONNew(/*parent*/NULL, /*after*/NULL, CUPS_JTYPE_OBJECT);
  cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "client_name"), client_name);
  if (client_uri)
    cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "client_uri"), client_uri);
  if (logo_uri)
    cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "logo_uri"), logo_uri);
  if (software_id)
    cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "software_id"), software_id);
  if (software_version)
    cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "software_version"), software_version);
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, /*after*/NULL, "redirect_uris"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, /*after*/NULL, redirect_uri);
  if (tos_uri)
    cupsJSONNewString(json, cupsJSONNewKey(json, /*after*/NULL, "tos_uri"), tos_uri);

  json_data = cupsJSONExportString(json);
  cupsJSONDelete(json);
  json = NULL;

  if (!json_data)
    return (NULL);

  json_length = strlen(json_data);

  // Send a POST request with the JSON data...
  if ((http = oauth_connect(registration_ep, host, sizeof(host), &port, resource, sizeof(resource))) == NULL)
    goto done;

  httpClearFields(http);
  httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "text/json");
  httpSetLength(http, json_length);

  if (!httpWriteRequest(http, "POST", resource))
  {
    if (httpReconnect(http, 30000, NULL))
      goto done;

    if (!httpWriteRequest(http, "POST", resource))
      goto done;
  }

  if (httpWrite(http, json_data, json_length) < json_length)
    goto done;

  free(json_data);
  json_data = NULL;

  while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE)
    ;

  json_data = oauth_copy_response(http);
  json      = cupsJSONImportString(json_data);

  if ((value = cupsJSONGetString(cupsJSONFind(json, "client_id"))) != NULL)
    client_id = strdup(value);
  else if ((value = cupsJSONGetString(cupsJSONFind(json, "error_description"))) != NULL)
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, value, false);
  else if ((value = cupsJSONGetString(cupsJSONFind(json, "error"))) != NULL)
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, value, false);
  else
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, httpStatusString(status), false);

  // Return whatever we got...
  done:

  httpClose(http);

  cupsJSONDelete(json);
  free(json_data);

  return (client_id);
}


//
// 'cupsOAuthDoToken()' - Obtain access and refresh tokens from a grant token.
//

char *					// O - Access token or `NULL` on error
cupsOAuthDoToken(
    cups_json_t *metadata,		// I - Authorization server metadata
    const char  *resource_uri,		// I - Resource URI
    const char  *redirect_uri,		// I - Redirect URI
    const char  *client_id,		// I - `client_id`
    const char  *code,			// I - Grant code from authorization response
    const char  *code_verifier,		// I - Code verifier string
    time_t      *access_expires)	// O - Expiration time for access token
{
  const char	*token_ep;		// Token endpoint
  http_t	*http = NULL;		// HTTP connection
  char		host[256],		// Token endpoint hostname
		resource[256];		// Token endpoint resource
  int		port;			// Token endpoint port
  http_status_t	status;			// Response status
  size_t	num_form = 0;		// Number of form variables
  cups_option_t	*form = NULL;		// Form variables
  char		*form_data = NULL;	// POST form data
  size_t	form_length;		// Length of data
  char		*json_data = NULL;	// JSON response data
  cups_json_t	*json = NULL;		// JSON variables
  char		*access_token = NULL;	// Access token


  // Range check input...
  if (access_expires)
    *access_expires = 0;

  if (!metadata || (token_ep = cupsJSONGetString(cupsJSONFind(metadata, "token_endpoint"))) == NULL || !redirect_uri || !client_id || !code)
    return (NULL);

  // Prepare form data to get an access token...
  num_form = cupsAddOption("grant_type", "authorization_code", num_form, &form);
  num_form = cupsAddOption("code", code, num_form, &form);
  num_form = cupsAddOption("redirect_uri", redirect_uri, num_form, &form);
  num_form = cupsAddOption("client_id", client_id, num_form, &form);

  if (code_verifier)
    num_form = cupsAddOption("code_verifier", code_verifier, num_form, &form);

  if ((form_data = cupsFormEncode(/*url*/NULL, num_form, form)) == NULL)
    goto done;

  form_length = strlen(form_data);

  // Send a POST request with the form data...
  if ((http = oauth_connect(token_ep, host, sizeof(host), &port, resource, sizeof(resource))) == NULL)
    goto done;

  httpClearFields(http);
  httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "application/x-www-form-urlencoded");
  httpSetLength(http, form_length);

  if (!httpWriteRequest(http, "POST", resource))
  {
    if (!httpReconnect(http, 30000, NULL))
      goto done;

    if (!httpWriteRequest(http, "POST", resource))
      goto done;
  }

  if (httpWrite(http, form_data, form_length) < form_length)
    goto done;

  while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);

  if (status == HTTP_STATUS_OK)
  {
    const char	*access_value = NULL,	// access_token
		*refresh_value = NULL;	// refresh_token
    double	expires_in;		// expires_in value
    time_t	access_expvalue;	// Expiration time for access_token

    json_data = oauth_copy_response(http);
    json      = cupsJSONImportString(json_data);

    access_value  = cupsJSONGetString(cupsJSONFind(json, "access_token"));
    expires_in    = cupsJSONGetNumber(cupsJSONFind(json, "expires_in"));
    refresh_value = cupsJSONGetString(cupsJSONFind(json, "refresh_token"));

    if (expires_in > 0.0)
      access_expvalue = time(NULL) + (long)expires_in;
    else
      access_expvalue = 0;

    cupsOAuthSetTokens(token_ep, resource_uri, access_value, access_expvalue, refresh_value);

    if (access_value)
      access_token = strdup(access_value);

    if (access_expires)
      *access_expires = access_expvalue;

    cupsJSONDelete(json);
    free(json_data);
  }
  else
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, httpStatusString(status), false);
  }

  // Return whatever we got...
  done:

  httpClose(http);

  cupsFreeOptions(num_form, form);
  free(form_data);

  return (access_token);
}


//
// 'cupsOAuthSetTokens()' - Save authorization and refresh tokens.
//

void
cupsOAuthSetTokens(
    const char *auth_uri,		// I - Authorization server FQDN
    const char *resource_uri,		// I - Resource server FQDN
    const char *access_token,		// I - Access token
    time_t     access_expires,		// I - Access expiration time
    const char *refresh_token)		// I - Refresh token
{
  char		filename[1024],		// Token filename
		temp[256];		// Temporary string
  int		fd;			// File descriptor


  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_ACCESS))
  {
    if (access_token)
    {
      // Save authorization token...
      if ((fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600)) >= 0)
      {
        write(fd, access_token, strlen(access_token));
	if (access_expires > 0)
	{
	  snprintf(temp, sizeof(temp), "\n%ld\n", (long)access_expires);
          write(fd, temp, strlen(temp));
	}
	close(fd);
      }
    }
    else
    {
      // Clear access token...
      unlink(filename);
    }
  }

  if (oauth_make_path(filename, sizeof(filename), auth_uri, resource_uri, _CUPS_OTYPE_REFRESH))
  {
    if (refresh_token)
    {
      // Save refresh token...
      if ((fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600)) >= 0)
      {
        write(fd, refresh_token, strlen(refresh_token));
	close(fd);
      }
    }
    else
    {
      // Clear refresh token...
      unlink(filename);
    }
  }
}


//
// 'oauth_connect()' - Connect to a server for a URI.
//

static http_t *				// O - HTTP connection
oauth_connect(const char *uri,		// I - URI
              char       *host,		// I - Hostname buffer
	      size_t     host_size,	// I - Size of hostname buffer
	      int        *port,		// O - Port
	      char       *resource,	// I - Resource buffer
	      size_t     resource_size)	// I - Size of resource buffer
{
  char		scheme[32],		// URI scheme
		userpass[256];		// Username:password data (not used)
  http_encryption_t encryption;		// Type of encryption to use
  http_t	*http;			// Connection to server


  // Separate the URI into its components...
  if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, host_size, port, resource, resource_size) < HTTP_URI_STATUS_OK)
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Bad URI."), true);
    return (NULL);
  }

  // Try connecting with the appropriate level of encryption...
  if (!strcmp(scheme, "https") || *port == 443)
    encryption = HTTP_ENCRYPTION_ALWAYS;
  else
    encryption = HTTP_ENCRYPTION_IF_REQUESTED;

  http = httpConnect(host, *port, /*addrlist*/NULL, AF_UNSPEC, encryption, /*blocking*/true, /*msec*/30000, /*cancel*/NULL);

  // TODO: Validate certificate trust

  // Return the connection...
  return (http);
}


//
// 'oauth_copy_response()' - Copy the response from a HTTP response.
//

static char *				// O - Response as a string
oauth_copy_response(http_t *http)	// I - HTTP connection
{
  char		*body,			// Message body data string
		*end,			// End of data
		*ptr;			// Pointer into string
  size_t	bodylen;		// Allocated length of string
  ssize_t	bytes;			// Bytes read
  http_state_t	initial_state;		// Initial HTTP state


  // Allocate memory for string...
  initial_state = httpGetState(http);

  if ((bodylen = httpGetLength(http)) == 0 || bodylen > 65536)
    bodylen = 65536;			// Accept up to 64k for GETs/POSTs

  if ((body = calloc(1, bodylen + 1)) != NULL)
  {
    for (ptr = body, end = body + bodylen; ptr < end; ptr += bytes)
    {
      if ((bytes = httpRead(http, ptr, end - ptr)) <= 0)
        break;
    }
  }

  if (httpGetState(http) == initial_state)
    httpFlush(http);

  return (body);
}


//
// 'oauth_make_path()' - Make an OAuth token path.
//

static char *				// O - Filename
oauth_make_path(
    char          *buffer,		// I - Filename buffer
    size_t        bufsize,		// I - Size of filename buffer
    const char    *auth_uri,		// I - Authorization server URI
    const char    *resource_uri,	// I - Resource URI
    _cups_otype_t otype)		// I - Type (_CUPS_OTYPE_xxx)
{
  char		auth_temp[1024],	// Temporary copy of auth_uri
		resource_temp[1024],	// Temporary copy of resource_uri
		*ptr;			// Pointer into temporary strings
  unsigned char	auth_hash[32],		// SHA-256 hash of base auth_uri
		resource_hash[32];	// SHA-256 hash of base resource_uri
  _cups_globals_t *cg = _cupsGlobals();	// Global data
  static const char * const otypes[] =	// Filename extensions for each type
  {
    "accs",
    "meta",
    "rfsh"
  };


  // Range check input...
  if (!auth_uri || strncmp(auth_uri, "https://", 8) || auth_uri[8] == '[' || isdigit(auth_uri[8] & 255) || (!resource_uri && otype != _CUPS_OTYPE_METADATA) || (resource_uri && strncmp(resource_uri, "https://", 8) && strncmp(resource_uri, "ipps://", 7)))
  {
    *buffer = '\0';
    return (NULL);
  }

  // First make sure the "oauth" directory exists...
  if (mkdir(cg->userconfig, 0700) && errno != EEXIST)
  {
    *buffer = '\0';
    return (NULL);
  }

  snprintf(buffer, bufsize, "%s/oauth", cg->userconfig);
  if (mkdir(buffer, 0700) && errno != EEXIST)
  {
    *buffer = '\0';
    return (NULL);
  }

  // Build the hashed versions of the auth and resource URIs...
  cupsCopyString(auth_temp, auth_uri + 8, sizeof(auth_temp));
  if ((ptr = strchr(auth_temp, '/')) != NULL)
    *ptr = '\0';			// Strip resource path
  if (!strchr(auth_temp, ':'))		// Add :443 if no port is present
    cupsConcatString(auth_temp, ":443", sizeof(auth_temp));

  cupsHashData("sha2-256", auth_temp, strlen(auth_temp), auth_hash, sizeof(auth_hash));
  cupsHashString(auth_hash, sizeof(auth_hash), auth_temp, sizeof(auth_temp));

  if (resource_uri)
  {
    if (!strncmp(resource_uri, "https://", 8))
    {
      // HTTPS URI
      cupsCopyString(resource_temp, resource_uri + 8, sizeof(resource_temp));
      if ((ptr = strchr(resource_temp, '/')) != NULL)
        *ptr = '\0';			// Strip resource path
      if (!strchr(resource_temp, ':'))	// Add :443 if no port is present
        cupsConcatString(resource_temp, ":443", sizeof(resource_temp));
    }
    else
    {
      // IPPS URI
      cupsCopyString(resource_temp, resource_uri + 7, sizeof(resource_temp));
      if ((ptr = strchr(resource_temp, '/')) != NULL)
        *ptr = '\0';			// Strip resource path
      if (!strchr(resource_temp, ':'))	// Add :631 if no port is present
        cupsConcatString(resource_temp, ":631", sizeof(resource_temp));
    }

    cupsHashData("sha2-256", resource_temp, strlen(resource_temp), resource_hash, sizeof(resource_hash));
    cupsHashString(resource_hash, sizeof(resource_hash), resource_temp, sizeof(resource_temp));
  }
  else
  {
    // Leave an empty string for the resource portion
    resource_temp[0] = '\0';
  }

  // Build the filename for the corresponding data...
  if (resource_temp[0])
    snprintf(buffer, bufsize, "%s/oauth/%s+%s.%s", cg->userconfig, auth_temp, resource_temp, otypes[otype]);
  else
    snprintf(buffer, bufsize, "%s/oauth/%s.%s", cg->userconfig, auth_temp, otypes[otype]);

  return (buffer);
}
