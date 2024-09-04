//
// File functions for CUPS.
//
// Since stdio files max out at 256 files on many systems, we have to
// write similar functions without this limit.  At the same time, using
// our own file functions allows us to provide transparent support of
// different line endings, gzip'd print files, etc.
//
// Copyright © 2021-2024 by OpenPrinting.
// Copyright © 2007-2019 by Apple Inc.
// Copyright © 1997-2007 by Easy Software Products, all rights reserved.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include "file-private.h"
#include "debug-internal.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>


//
// Internal structures...
//

struct _cups_file_s			// CUPS file structure...
{
  int		fd;			// File descriptor
  bool		compressed;		// Compression used?
  char		mode,			// Mode ('r' or 'w')
		buf[4096],		// Buffer
		*ptr,			// Pointer into buffer
		*end;			// End of buffer data
  bool		is_stdio,		// stdin/out/err?
		eof;			// End of file?
  off_t		pos,			// Position in file
		bufpos;			// File position for start of buffer

  z_stream	stream;			// (De)compression stream
  Bytef		cbuf[4096];		// (De)compression buffer
  uLong		crc;			// (De)compression CRC

  char		*printf_buffer;		// cupsFilePrintf buffer
  size_t	printf_size;		// Size of cupsFilePrintf buffer
};


//
// Local functions...
//

static bool	cups_compress(cups_file_t *fp, const char *buf, size_t bytes);
static ssize_t	cups_fill(cups_file_t *fp);
static int	cups_open(const char *filename, int oflag, int mode);
static ssize_t	cups_read(cups_file_t *fp, char *buf, size_t bytes);
static bool	cups_write(cups_file_t *fp, const char *buf, size_t bytes);


//
// 'cupsFileClose()' - Close a CUPS file.
//

bool					// O - `true` on success, `false` on error
cupsFileClose(cups_file_t *fp)		// I - CUPS file
{
  int	fd;				// File descriptor
  char	mode;				// Open mode
  bool	status;				// Return status


  // Range check...
  if (!fp)
    return (false);

  // Flush pending write data...
  if (fp->mode == 'w')
    status = cupsFileFlush(fp);
  else
    status = true;

  if (fp->compressed && status)
  {
    if (fp->mode == 'r')
    {
      // Free decompression data...
      inflateEnd(&fp->stream);
    }
    else
    {
      // Flush any remaining compressed data...
      unsigned char	trailer[8];	// Trailer CRC and length
      bool		done;		// Done writing...


      fp->stream.avail_in = 0;

      for (done = false;;)
      {
        if (fp->stream.next_out > fp->cbuf)
	{
	  status = cups_write(fp, (char *)fp->cbuf, (size_t)(fp->stream.next_out - fp->cbuf));

	  fp->stream.next_out  = fp->cbuf;
	  fp->stream.avail_out = sizeof(fp->cbuf);
	}

        if (done || !status)
	  break;

        done = deflate(&fp->stream, Z_FINISH) == Z_STREAM_END && fp->stream.next_out == fp->cbuf;
      }

      // Write the CRC and length...
      trailer[0] = (unsigned char)fp->crc;
      trailer[1] = (unsigned char)(fp->crc >> 8);
      trailer[2] = (unsigned char)(fp->crc >> 16);
      trailer[3] = (unsigned char)(fp->crc >> 24);
      trailer[4] = (unsigned char)fp->pos;
      trailer[5] = (unsigned char)(fp->pos >> 8);
      trailer[6] = (unsigned char)(fp->pos >> 16);
      trailer[7] = (unsigned char)(fp->pos >> 24);

      status = cups_write(fp, (char *)trailer, 8);

      // Free all memory used by the compression stream...
      deflateEnd(&(fp->stream));
    }
  }

  // If this is one of the cupsFileStdin/out/err files, return now and don't
  // actually free memory or close (these last the life of the process...)
  if (fp->is_stdio)
    return (status);

  // Save the file descriptor we used and free memory...
  fd   = fp->fd;
  mode = fp->mode;

  if (fp->printf_buffer)
    free(fp->printf_buffer);

  free(fp);

  // Close the file, returning the close status...
  if (mode == 's')
    status = httpAddrClose(NULL, fd);
  else if (close(fd) < 0)
    status = false;

  return (status);
}


//
// 'cupsFileEOF()' - Return the end-of-file status.
//

bool					// O - `true` on end of file, `false` otherwise
cupsFileEOF(cups_file_t *fp)		// I - CUPS file
{
  return (fp ? fp->eof : true);
}


//
// 'cupsFileFind()' - Find a file using the specified path.
//
// This function allows the paths in the path string to be separated by
// colons (POSIX standard) or semicolons (Windows standard) and stores the
// result in the buffer supplied.  If the file cannot be found in any of
// the supplied paths, `NULL` is returned.  A `NULL` path only matches the
// current directory.
//

const char *				// O - Full path to file or `NULL` if not found
cupsFileFind(const char *filename,	// I - File to find
             const char *path,		// I - Colon/semicolon-separated path
             bool       executable,	// I - `true` = executable files, `false` = any file/dir
	     char       *buffer,	// I - Filename buffer
	     size_t     bufsize)	// I - Size of filename buffer
{
  char	*bufptr,			// Current position in buffer
	*bufend;			// End of buffer


  // Range check input...
  if (!filename || !buffer || bufsize < 2)
    return (NULL);

  if (!path)
  {
    // No path, so check current directory...
    if (!access(filename, 0))
    {
      cupsCopyString(buffer, filename, (size_t)bufsize);
      return (buffer);
    }
    else
    {
      return (NULL);
    }
  }

  // Now check each path and return the first match...
  bufend = buffer + bufsize - 1;
  bufptr = buffer;

  while (*path)
  {
#ifdef _WIN32
    if (*path == ';' || (*path == ':' && ((bufptr - buffer) > 1 || !isalpha(buffer[0] & 255))))
#else
    if (*path == ';' || *path == ':')
#endif // _WIN32
    {
      if (bufptr > buffer && bufptr[-1] != '/' && bufptr < bufend)
        *bufptr++ = '/';

      cupsCopyString(bufptr, filename, (size_t)(bufend - bufptr));

#ifdef _WIN32
      if (!access(buffer, 0))
#else
      if (!access(buffer, executable ? X_OK : 0))
#endif // _WIN32
        return (buffer);

      bufptr = buffer;
    }
    else if (bufptr < bufend)
      *bufptr++ = *path;

    path ++;
  }

  // Check the last path...
  if (bufptr > buffer && bufptr[-1] != '/' && bufptr < bufend)
    *bufptr++ = '/';

  cupsCopyString(bufptr, filename, (size_t)(bufend - bufptr));

  if (!access(buffer, 0))
    return (buffer);
  else
    return (NULL);
}


//
// 'cupsFileFlush()' - Flush pending output.
//

bool					// O - `true` on success, `false` on error
cupsFileFlush(cups_file_t *fp)		// I - CUPS file
{
  ssize_t	bytes;			// Bytes to write
  bool		ret;			// Return value


  // Range check input...
  if (!fp || fp->mode != 'w')
    return (false);

  bytes = (ssize_t)(fp->ptr - fp->buf);

  if (bytes > 0)
  {
    if (fp->compressed)
      ret = cups_compress(fp, fp->buf, (size_t)bytes);
    else
      ret = cups_write(fp, fp->buf, (size_t)bytes);

    fp->ptr = fp->buf;

    return (ret);
  }

  return (true);
}


//
// 'cupsFileGetChar()' - Get a single character from a file.
//

int					// O - Character or `-1` on end of file
cupsFileGetChar(cups_file_t *fp)	// I - CUPS file
{
  // Range check input...
  if (!fp || (fp->mode != 'r' && fp->mode != 's'))
    return (-1);

  if (fp->eof)
    return (-1);

  // If the input buffer is empty, try to read more data...
  if (fp->ptr >= fp->end)
  {
    if (cups_fill(fp) <= 0)
      return (-1);
  }

  // Return the next character in the buffer...
  fp->pos ++;

  return (*(fp->ptr)++ & 255);
}


//
// 'cupsFileGetConf()' - Get a line from a configuration file.
//

char *					// O  - Line read or `NULL` on end of file or error
cupsFileGetConf(cups_file_t *fp,	// I  - CUPS file
                char        *buf,	// O  - String buffer
		size_t      buflen,	// I  - Size of string buffer
                char        **value,	// O  - Pointer to value
		int         *linenum)	// IO - Current line number
{
  char	*ptr;				// Pointer into line


  // Range check input...
  if (!fp || (fp->mode != 'r' && fp->mode != 's') ||
      !buf || buflen < 2 || !value)
  {
    if (value)
      *value = NULL;

    return (NULL);
  }

  // Read the next non-comment line...
  *value = NULL;

  while (cupsFileGets(fp, buf, buflen))
  {
    (*linenum) ++;

    // Strip any comments...
    if ((ptr = strchr(buf, '#')) != NULL)
    {
      if (ptr > buf && ptr[-1] == '\\')
      {
        // Unquote the #...
	_cups_strcpy(ptr - 1, ptr);
      }
      else
      {
        // Strip the comment and any trailing whitespace...
	while (ptr > buf)
	{
	  if (!_cups_isspace(ptr[-1]))
	    break;

	  ptr --;
	}

	*ptr = '\0';
      }
    }

    // Strip leading whitespace...
    for (ptr = buf; _cups_isspace(*ptr); ptr ++);

    if (ptr > buf)
      _cups_strcpy(buf, ptr);

    // See if there is anything left...
    if (buf[0])
    {
      // Yes, grab any value and return...
      for (ptr = buf; *ptr; ptr ++)
      {
        if (_cups_isspace(*ptr))
	  break;
      }

      if (*ptr)
      {
        // Have a value, skip any other spaces...
        while (_cups_isspace(*ptr))
	  *ptr++ = '\0';

        if (*ptr)
	  *value = ptr;

        // Strip trailing whitespace and > for lines that begin with <...
        ptr += strlen(ptr) - 1;

        if (buf[0] == '<' && *ptr == '>')
        {
	  *ptr-- = '\0';
	}
	else if (buf[0] == '<' && *ptr != '>')
        {
	  // Syntax error...
	  *value = NULL;
	  return (buf);
	}

        while (ptr > *value && _cups_isspace(*ptr))
	  *ptr-- = '\0';
      }

      // Return the line...
      return (buf);
    }
  }

  return (NULL);
}


//
// 'cupsFileGetLine()' - Get a CR and/or LF-terminated line that may
//                       contain binary data.
//
// This function differs from @link cupsFileGets@ in that the trailing CR
// and LF are preserved, as is any binary data on the line. The buffer is
// `nul`-terminated, however you should use the returned length to determine
// the number of bytes on the line.
//

size_t					// O - Number of bytes on line or 0 on end of file
cupsFileGetLine(cups_file_t *fp,	// I - File to read from
                char        *buf,	// I - Buffer
                size_t      buflen)	// I - Size of buffer
{
  int		ch;			// Character from file
  char		*ptr,			// Current position in line buffer
		*end;			// End of line buffer


  // Range check input...
  if (!fp || (fp->mode != 'r' && fp->mode != 's') || !buf || buflen < 3)
    return (0);

  // Now loop until we have a valid line...
  for (ptr = buf, end = buf + buflen - 2; ptr < end ;)
  {
    if (fp->ptr >= fp->end)
    {
      if (cups_fill(fp) <= 0)
        break;
    }

    *ptr++ = ch = *(fp->ptr)++;
    fp->pos ++;

    if (ch == '\r')
    {
      // Check for CR LF...
      if (fp->ptr >= fp->end)
      {
	if (cups_fill(fp) <= 0)
          break;
      }

      if (*(fp->ptr) == '\n')
      {
        *ptr++ = *(fp->ptr)++;
	fp->pos ++;
      }

      break;
    }
    else if (ch == '\n')
    {
      // Line feed ends a line...
      break;
    }
  }

  *ptr = '\0';

  return ((size_t)(ptr - buf));
}


//
// 'cupsFileGets()' - Get a CR and/or LF-terminated line.
//

char *					// O - Line read or `NULL` on end of file or error
cupsFileGets(cups_file_t *fp,		// I - CUPS file
             char        *buf,		// O - String buffer
	     size_t      buflen)	// I - Size of string buffer
{
  int		ch;			// Character from file
  char		*ptr,			// Current position in line buffer
		*end;			// End of line buffer


  // Range check input...
  if (!fp || (fp->mode != 'r' && fp->mode != 's') || !buf || buflen < 2)
    return (NULL);

  // Now loop until we have a valid line...
  for (ptr = buf, end = buf + buflen - 1; ptr < end ;)
  {
    if (fp->ptr >= fp->end)
    {
      if (cups_fill(fp) <= 0)
      {
        if (ptr == buf)
	  return (NULL);
	else
          break;
      }
    }

    ch = *(fp->ptr)++;
    fp->pos ++;

    if (ch == '\r')
    {
      // Check for CR LF...
      if (fp->ptr >= fp->end)
      {
	if (cups_fill(fp) <= 0)
          break;
      }

      if (*(fp->ptr) == '\n')
      {
        fp->ptr ++;
	fp->pos ++;
      }

      break;
    }
    else if (ch == '\n')
    {
      // Line feed ends a line...
      break;
    }
    else
    {
      *ptr++ = (char)ch;
    }
  }

  *ptr = '\0';

  return (buf);
}


//
// 'cupsFileIsCompressed()' - Return whether a file is compressed.
//

bool					// O - `true` if compressed, `false` if not
cupsFileIsCompressed(cups_file_t *fp)	// I - CUPS file
{
  return (fp ? fp->compressed : false);
}


//
// 'cupsFileLock()' - Temporarily lock access to a file.
//

bool					// O - `true` on success, `false` on error
cupsFileLock(cups_file_t *fp,		// I - CUPS file
             bool        block)		// I - `true` to wait for the lock, `false` to fail right away
{
  // Range check...
  if (!fp || fp->mode == 's')
    return (false);

  // Try the lock...
#ifdef _WIN32
  return (_locking(fp->fd, block ? _LK_LOCK : _LK_NBLCK, 0) == 0);
#else
  return (lockf(fp->fd, block ? F_LOCK : F_TLOCK, 0) == 0);
#endif // _WIN32
}


//
// 'cupsFileNumber()' - Return the file descriptor associated with a CUPS file.
//

int					// O - File descriptor
cupsFileNumber(cups_file_t *fp)		// I - CUPS file
{
  if (fp)
    return (fp->fd);
  else
    return (-1);
}


//
// 'cupsFileOpen()' - Open a CUPS file.
//
// This function opens a file or socket for use with the CUPS file API.
//
// The "filename" argument is a filename or socket address.
//
// The "mode" parameter can be "r" to read, "w" to write, overwriting any
// existing file, "a" to append to an existing file or create a new file,
// or "s" to open a socket connection.
//
// When opening for writing ("w"), an optional number from `1` to `9` can be
// supplied which enables Flate compression of the file.  Compression is
// not supported for the "a" (append) mode.
//
// When opening for writing ("w") or append ("a"), an optional 'm###' suffix
// can be used to set the permissions of the opened file.
//
// When opening a socket connection, the filename is a string of the form
// "address:port" or "hostname:port". The socket will make an IPv4 or IPv6
// connection as needed, generally preferring IPv6 connections when there is
// a choice.
//

cups_file_t *				// O - CUPS file or `NULL` if the file or socket cannot be opened
cupsFileOpen(const char *filename,	// I - Name of file
             const char *mode)		// I - Open mode
{
  cups_file_t	*fp;			// New CUPS file
  int		fd;			// File descriptor
  char		hostname[1024],		// Hostname
		*portname;		// Port "name" (number or service)
  http_addrlist_t *addrlist;		// Host address list
  int		perm = 0664;		// Permissions for write/append
  const char	*ptr;			// Pointer into mode string


  // Range check input...
  if (!filename || !mode || (*mode != 'r' && *mode != 'w' && *mode != 'a' && *mode != 's') || (*mode == 'a' && isdigit(mode[1] & 255)))
    return (NULL);

  if ((ptr = strchr(mode, 'm')) != NULL && ptr[1] >= '0' && ptr[1] <= '7')
  {
    // Get permissions from mode string...
    perm = (int)strtol(mode + 1, NULL, 8);
  }

  // Open the file...
  switch (*mode)
  {
    case 'a' : // Append file
        fd = cups_open(filename, O_WRONLY | O_CREAT | O_APPEND | O_LARGEFILE | O_BINARY, perm);
        break;

    case 'r' : // Read file
	fd = open(filename, O_RDONLY | O_LARGEFILE | O_BINARY, 0);
	break;

    case 'w' : // Write file
        fd = cups_open(filename, O_WRONLY | O_LARGEFILE | O_BINARY, perm);
	if (fd < 0 && errno == ENOENT)
	{
	  fd = cups_open(filename, O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE | O_BINARY, perm);
	  if (fd < 0 && errno == EEXIST)
	    fd = cups_open(filename, O_WRONLY | O_LARGEFILE | O_BINARY, perm);
	}

	if (fd >= 0)
#ifdef _WIN32
	  _chsize(fd, 0);
#else
	  ftruncate(fd, 0);
#endif // _WIN32
        break;

    case 's' : // Read/write socket
        cupsCopyString(hostname, filename, sizeof(hostname));
	if ((portname = strrchr(hostname, ':')) != NULL)
	  *portname++ = '\0';
	else
	  return (NULL);

        // Lookup the hostname and service...
        if ((addrlist = httpAddrGetList(hostname, AF_UNSPEC, portname)) == NULL)
	  return (NULL);

        // Connect to the server...
        if (!httpAddrConnect(addrlist, &fd, 30000, NULL))
	{
	  httpAddrFreeList(addrlist);
	  return (NULL);
	}

	httpAddrFreeList(addrlist);
	break;

    default : // Remove bogus compiler warning...
        return (NULL);
  }

  if (fd < 0)
    return (NULL);

  // Create the CUPS file structure...
  if ((fp = cupsFileOpenFd(fd, mode)) == NULL)
  {
    if (*mode == 's')
      httpAddrClose(NULL, fd);
    else
      close(fd);
  }

  // Return it...
  return (fp);
}


//
// 'cupsFileOpenFd()' - Open a CUPS file using a file descriptor.
//
// This function prepares a file descriptor for use with the CUPS file API.
//
// The "fd" argument specifies the file descriptor.
//
// The "mode" argument can be "r" to read, "w" to write, "a" to append,
// or "s" to treat the file descriptor as a bidirectional socket connection.
//
// When opening for writing ("w"), an optional number from `1` to `9` can be
// supplied which enables Flate compression of the file.  Compression is
// not supported for the "a" (append) mode.
//

cups_file_t *				// O - CUPS file or `NULL` if the file could not be opened
cupsFileOpenFd(int        fd,		// I - File descriptor
	       const char *mode)	// I - Open mode
{
  cups_file_t	*fp;			// New CUPS file


  // Range check input...
  if (fd < 0 || !mode || (*mode != 'r' && *mode != 'w' && *mode != 'a' && *mode != 's') || (*mode == 'a' && isdigit(mode[1] & 255)))
    return (NULL);

  // Allocate memory...
  if ((fp = calloc(1, sizeof(cups_file_t))) == NULL)
    return (NULL);

  // Open the file...
  fp->fd = fd;

  switch (*mode)
  {
    case 'a' :
    case 'w' :
        if (*mode == 'a')
          fp->pos = lseek(fd, 0, SEEK_END);

	fp->mode = 'w';
	fp->ptr  = fp->buf;
	fp->end  = fp->buf + sizeof(fp->buf);

	if (mode[1] >= '1' && mode[1] <= '9')
	{
	  // Open a compressed stream, so write the standard gzip file header...
          unsigned char header[10];	// gzip file header
	  time_t	curtime;	// Current time


          curtime   = time(NULL);
	  header[0] = 0x1f;
	  header[1] = 0x8b;
	  header[2] = Z_DEFLATED;
	  header[3] = 0;
	  header[4] = (unsigned char)curtime;
	  header[5] = (unsigned char)(curtime >> 8);
	  header[6] = (unsigned char)(curtime >> 16);
	  header[7] = (unsigned char)(curtime >> 24);
	  header[8] = 0;
	  header[9] = 0x03;

	  cups_write(fp, (char *)header, 10);

          // Initialize the compressor...
          if (deflateInit2(&(fp->stream), mode[1] - '0', Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) < Z_OK)
          {
            free(fp);
	    return (NULL);
          }

	  fp->stream.next_out  = fp->cbuf;
	  fp->stream.avail_out = sizeof(fp->cbuf);
	  fp->compressed       = true;
	  fp->crc              = crc32(0L, Z_NULL, 0);
	}
        break;

    case 'r' :
	fp->mode = 'r';
	break;

    case 's' :
        fp->mode = 's';
	break;

    default : // Remove bogus compiler warning...
        return (NULL);
  }

  // Don't pass this file to child processes...
#ifndef _WIN32
  if (fcntl(fp->fd, F_SETFD, fcntl(fp->fd, F_GETFD) | FD_CLOEXEC))
    DEBUG_printf("cupsFileOpenFd: fcntl(F_SETFD, FD_CLOEXEC) failed - %s", strerror(errno));
#endif // !_WIN32

  return (fp);
}


//
// '_cupsFilePeekAhead()' - See if the requested character is buffered up.
//

bool					// O - `true` if present, `false` otherwise
_cupsFilePeekAhead(cups_file_t *fp,	// I - CUPS file
                   int         ch)	// I - Character
{
  return (fp && fp->ptr && memchr(fp->ptr, ch, (size_t)(fp->end - fp->ptr)));
}


//
// 'cupsFilePeekChar()' - Peek at the next character from a file.
//

int					// O - Character or `-1` on end of file
cupsFilePeekChar(cups_file_t *fp)	// I - CUPS file
{
  // Range check input...
  if (!fp || (fp->mode != 'r' && fp->mode != 's'))
    return (-1);

  // If the input buffer is empty, try to read more data...
  if (fp->ptr >= fp->end)
  {
    if (cups_fill(fp) <= 0)
      return (-1);
  }

  // Return the next character in the buffer...
  return (*(fp->ptr) & 255);
}


//
// 'cupsFilePrintf()' - Write a formatted string.
//

bool					// O - `true` on success, `false` on error
cupsFilePrintf(cups_file_t *fp,		// I - CUPS file
               const char  *format,	// I - Printf-style format string
	       ...)			// I - Additional args as necessary
{
  va_list	ap;			// Argument list
  ssize_t	bytes;			// Formatted size


  if (!fp || !format || (fp->mode != 'w' && fp->mode != 's'))
    return (false);

  if (!fp->printf_buffer)
  {
    // Start with an 1k printf buffer...
    if ((fp->printf_buffer = malloc(1024)) == NULL)
      return (false);

    fp->printf_size = 1024;
  }

  // TODO: Use va_copy instead of calling va_start with the same args
  va_start(ap, format);
  bytes = vsnprintf(fp->printf_buffer, fp->printf_size, format, ap);
  va_end(ap);

  if (bytes >= (ssize_t)fp->printf_size)
  {
    // Expand the printf buffer...
    char	*temp;			// Temporary buffer pointer

    if (bytes > 65535)
      return (-1);

    if ((temp = realloc(fp->printf_buffer, (size_t)(bytes + 1))) == NULL)
      return (-1);

    fp->printf_buffer = temp;
    fp->printf_size   = (size_t)(bytes + 1);

    va_start(ap, format);
    bytes = vsnprintf(fp->printf_buffer, fp->printf_size, format, ap);
    va_end(ap);
  }

  if (fp->mode == 's')
  {
    if (!cups_write(fp, fp->printf_buffer, (size_t)bytes))
      return (false);

    fp->pos += bytes;

    return ((int)bytes);
  }

  if ((fp->ptr + bytes) > fp->end)
  {
    if (!cupsFileFlush(fp))
      return (false);
  }

  fp->pos += bytes;

  if ((size_t)bytes > sizeof(fp->buf))
  {
    if (fp->compressed)
      return (cups_compress(fp, fp->printf_buffer, (size_t)bytes));
    else
      return (cups_write(fp, fp->printf_buffer, (size_t)bytes));
  }
  else
  {
    memcpy(fp->ptr, fp->printf_buffer, (size_t)bytes);
    fp->ptr += bytes;

    if (fp->is_stdio && !cupsFileFlush(fp))
      return (false);
    else
      return (true);
  }
}


//
// 'cupsFilePutChar()' - Write a character.
//

bool					// O - `true` on success, `false` on error
cupsFilePutChar(cups_file_t *fp,	// I - CUPS file
                int         c)		// I - Character to write
{
  // Range check input...
  if (!fp || (fp->mode != 'w' && fp->mode != 's'))
    return (false);

  if (fp->mode == 's')
  {
    // Send character immediately over socket...
    char ch;				// Output character

    ch = (char)c;

    if (send(fp->fd, &ch, 1, 0) < 1)
      return (false);
  }
  else
  {
    // Buffer it up...
    if (fp->ptr >= fp->end)
    {
      if (!cupsFileFlush(fp))
	return (false);
    }

    *(fp->ptr) ++ = (char)c;
  }

  fp->pos ++;

  return (true);
}


//
// 'cupsFilePutConf()' - Write a configuration line.
//
// This function handles any comment escaping of the value.
//

bool					// O - `true` on success, `false` on error
cupsFilePutConf(cups_file_t *fp,	// I - CUPS file
                const char *directive,	// I - Directive
		const char *value)	// I - Value
{
  const char	*ptr;			// Pointer into value


  if (!fp || !directive || !*directive)
    return (false);

  if (!cupsFilePuts(fp, directive))
    return (false);

  if (!cupsFilePutChar(fp, ' '))
    return (false);

  if (value && *value)
  {
    if ((ptr = strchr(value, '#')) != NULL)
    {
      // Need to quote the first # in the info string...
      if (!cupsFileWrite(fp, value, (size_t)(ptr - value)))
        return (false);

      if (!cupsFilePutChar(fp, '\\'))
        return (false);

      if (!cupsFilePuts(fp, ptr))
        return (false);
    }
    else if (!cupsFilePuts(fp, value))
    {
      return (false);
    }
  }

  return (cupsFilePutChar(fp, '\n'));
}


//
// 'cupsFilePuts()' - Write a string.
//
// Like the `fputs` function, no newline is appended to the string.
//

bool					// O - `true` on success, `false` on error
cupsFilePuts(cups_file_t *fp,		// I - CUPS file
             const char  *s)		// I - String to write
{
  size_t	bytes;			// Bytes to write


  // Range check input...
  if (!fp || !s || (fp->mode != 'w' && fp->mode != 's'))
    return (false);

  // Write the string...
  bytes = strlen(s);

  if (fp->mode == 's')
  {
    if (!cups_write(fp, s, bytes))
      return (false);

    fp->pos += bytes;

    return (true);
  }

  if ((fp->ptr + bytes) > fp->end)
  {
    if (!cupsFileFlush(fp))
      return (false);
  }

  fp->pos += bytes;

  if (bytes > sizeof(fp->buf))
  {
    if (fp->compressed)
      return (cups_compress(fp, s, bytes) > 0);
    else
      return (cups_write(fp, s, bytes) > 0);
  }
  else
  {
    memcpy(fp->ptr, s, bytes);
    fp->ptr += bytes;

    if (fp->is_stdio && !cupsFileFlush(fp))
      return (false);
    else
      return (true);
  }
}


//
// 'cupsFileRead()' - Read from a file.
//

ssize_t					// O - Number of bytes read or -1 on error
cupsFileRead(cups_file_t *fp,		// I - CUPS file
             char        *buf,		// O - Buffer
	     size_t      bytes)		// I - Number of bytes to read
{
  size_t	total;			// Total bytes read
  ssize_t	count;			// Bytes read


  // Range check input...
  if (!fp || !buf || (fp->mode != 'r' && fp->mode != 's'))
    return (-1);

  if (bytes == 0)
    return (0);

  if (fp->eof)
    return (-1);

  // Loop until all bytes are read...
  total = 0;
  while (bytes > 0)
  {
    if (fp->ptr >= fp->end)
    {
      if (cups_fill(fp) <= 0)
      {
        if (total > 0)
          return ((ssize_t)total);
	else
	  return (-1);
      }
    }

    count = (ssize_t)(fp->end - fp->ptr);
    if (count > (ssize_t)bytes)
      count = (ssize_t)bytes;

    memcpy(buf, fp->ptr,(size_t) count);
    fp->ptr += count;
    fp->pos += count;

    // Update the counts for the last read...
    bytes -= (size_t)count;
    total += (size_t)count;
    buf   += count;
  }

  // Return the total number of bytes read...
  return ((ssize_t)total);
}


//
// 'cupsFileRewind()' - Set the current file position to the beginning of the file.
//

off_t					// O - New file position or `-1` on error
cupsFileRewind(cups_file_t *fp)		// I - CUPS file
{
  // Range check input...
  if (!fp || fp->mode != 'r')
    return (-1);

  // Handle special cases...
  if (fp->bufpos == 0)
  {
    // No seeking necessary...
    fp->pos = 0;

    if (fp->ptr)
    {
      fp->ptr = fp->buf;
      fp->eof = false;
    }

    return (0);
  }

  // Otherwise, seek in the file and cleanup any compression buffers...
  if (fp->compressed)
  {
    inflateEnd(&fp->stream);
    fp->compressed = false;
  }

  if (lseek(fp->fd, 0, SEEK_SET))
    return (-1);

  fp->bufpos = 0;
  fp->pos    = 0;
  fp->ptr    = NULL;
  fp->end    = NULL;
  fp->eof    = false;

  return (0);
}


//
// 'cupsFileSeek()' - Seek in a file.
//

off_t					// O - New file position or `-1` on error
cupsFileSeek(cups_file_t *fp,		// I - CUPS file
             off_t       pos)		// I - Position in file
{
  ssize_t	bytes;			// Number bytes in buffer


  // Range check input...
  if (!fp || pos < 0 || fp->mode != 'r')
    return (-1);

  // Handle special cases...
  if (pos == 0)
    return (cupsFileRewind(fp));

  if (fp->ptr)
  {
    bytes = (ssize_t)(fp->end - fp->buf);

    if (pos >= fp->bufpos && pos < (fp->bufpos + bytes))
    {
      // No seeking necessary...
      fp->pos = pos;
      fp->ptr = fp->buf + (pos - fp->bufpos);
      fp->eof = false;

      return (pos);
    }
  }

  if (!fp->compressed && !fp->ptr)
  {
    // Preload a buffer to determine whether the file is compressed...
    if (cups_fill(fp) <= 0)
      return (-1);
  }

  // Seek forwards or backwards...
  fp->eof = false;

  if (pos < fp->bufpos)
  {
    // Need to seek backwards...
    if (fp->compressed)
    {
      inflateEnd(&fp->stream);

      lseek(fp->fd, 0, SEEK_SET);
      fp->bufpos = 0;
      fp->pos    = 0;
      fp->ptr    = NULL;
      fp->end    = NULL;

      while ((bytes = cups_fill(fp)) > 0)
      {
        if (pos >= fp->bufpos && pos < (fp->bufpos + bytes))
	  break;
      }

      if (bytes <= 0)
        return (-1);

      fp->ptr = fp->buf + pos - fp->bufpos;
      fp->pos = pos;
    }
    else
    {
      fp->bufpos = lseek(fp->fd, pos, SEEK_SET);
      fp->pos    = fp->bufpos;
      fp->ptr    = NULL;
      fp->end    = NULL;
    }
  }
  else
  {
    // Need to seek forwards...
    if (fp->compressed)
    {
      while ((bytes = cups_fill(fp)) > 0)
      {
        if (pos >= fp->bufpos && pos < (fp->bufpos + bytes))
	  break;
      }

      if (bytes <= 0)
        return (-1);

      fp->ptr = fp->buf + pos - fp->bufpos;
      fp->pos = pos;
    }
    else
    {
      fp->bufpos = lseek(fp->fd, pos, SEEK_SET);
      fp->pos    = fp->bufpos;
      fp->ptr    = NULL;
      fp->end    = NULL;
    }
  }

  return (fp->pos);
}


//
// 'cupsFileStderr()' - Return a CUPS file associated with stderr.
//

cups_file_t *				// O - CUPS file
cupsFileStderr(void)
{
  _cups_globals_t *cg = _cupsGlobals();	// Pointer to library globals...


  // Open file descriptor 2 as needed...
  if (!cg->stdio_files[2])
  {
    // Flush any pending output on the stdio file...
    fflush(stderr);

    // Open file descriptor 2...
    if ((cg->stdio_files[2] = cupsFileOpenFd(2, "w")) != NULL)
      cg->stdio_files[2]->is_stdio = true;
  }

  return (cg->stdio_files[2]);
}


//
// 'cupsFileStdin()' - Return a CUPS file associated with stdin.
//

cups_file_t *				// O - CUPS file
cupsFileStdin(void)
{
  _cups_globals_t *cg = _cupsGlobals();	// Pointer to library globals...


  // Open file descriptor 0 as needed...
  if (!cg->stdio_files[0])
  {
    // Open file descriptor 0...
    if ((cg->stdio_files[0] = cupsFileOpenFd(0, "r")) != NULL)
      cg->stdio_files[0]->is_stdio = true;
  }

  return (cg->stdio_files[0]);
}


//
// 'cupsFileStdout()' - Return a CUPS file associated with stdout.
//

cups_file_t *				// O - CUPS file
cupsFileStdout(void)
{
  _cups_globals_t *cg = _cupsGlobals();	// Pointer to library globals...


  // Open file descriptor 1 as needed...
  if (!cg->stdio_files[1])
  {
    // Flush any pending output on the stdio file...
    fflush(stdout);

    // Open file descriptor 1...
    if ((cg->stdio_files[1] = cupsFileOpenFd(1, "w")) != NULL)
      cg->stdio_files[1]->is_stdio = true;
  }

  return (cg->stdio_files[1]);
}


//
// 'cupsFileTell()' - Return the current file position.
//

off_t					// O - File position
cupsFileTell(cups_file_t *fp)		// I - CUPS file
{
  return (fp ? fp->pos : 0);
}


//
// 'cupsFileUnlock()' - Unlock access to a file.
//

bool					// O - `true` on success, `false` on error
cupsFileUnlock(cups_file_t *fp)		// I - CUPS file
{
  // Range check...
  if (!fp || fp->mode == 's')
    return (false);

  // Unlock...
#ifdef _WIN32
  return (_locking(fp->fd, _LK_UNLCK, 0) == 0);
#else
  return (lockf(fp->fd, F_ULOCK, 0) == 0);
#endif // _WIN32
}


//
// 'cupsFileWrite()' - Write to a file.
//

bool					// O - `true` on success, `false` on error
cupsFileWrite(cups_file_t *fp,		// I - CUPS file
              const char  *buf,		// I - Buffer
	      size_t      bytes)	// I - Number of bytes to write
{
  // Range check input...
  if (!fp || !buf || (fp->mode != 'w' && fp->mode != 's'))
    return (false);

  if (bytes == 0)
    return (true);

  // Write the buffer...
  if (fp->mode == 's')
  {
    if (!cups_write(fp, buf, bytes))
      return (false);

    fp->pos += (off_t)bytes;

    return (true);
  }

  if ((fp->ptr + bytes) > fp->end)
  {
    if (!cupsFileFlush(fp))
      return (false);
  }

  fp->pos += (off_t)bytes;

  if (bytes > sizeof(fp->buf))
  {
    if (fp->compressed)
      return (cups_compress(fp, buf, bytes));
    else
      return (cups_write(fp, buf, bytes));
  }
  else
  {
    memcpy(fp->ptr, buf, bytes);
    fp->ptr += bytes;
    return (true);
  }
}


//
// 'cups_compress()' - Compress a buffer of data.
//

static bool				// O - `true` on success, `false` on error
cups_compress(cups_file_t *fp,		// I - CUPS file
              const char  *buf,		// I - Buffer
	      size_t      bytes)	// I - Number bytes
{
  int	status;				// Deflate status


  // Update the CRC...
  fp->crc = crc32(fp->crc, (const Bytef *)buf, (uInt)bytes);

  // Deflate the bytes...
  fp->stream.next_in  = (Bytef *)buf;
  fp->stream.avail_in = (uInt)bytes;

  while (fp->stream.avail_in > 0)
  {
    // Flush the current buffer...
    if (fp->stream.avail_out < (uInt)(sizeof(fp->cbuf) / 8))
    {
      if (!cups_write(fp, (char *)fp->cbuf, (size_t)(fp->stream.next_out - fp->cbuf)))
        return (false);

      fp->stream.next_out  = fp->cbuf;
      fp->stream.avail_out = sizeof(fp->cbuf);
    }

    if ((status = deflate(&(fp->stream), Z_NO_FLUSH)) < Z_OK && status != Z_BUF_ERROR)
      return (false);
  }

  return (true);
}


//
// 'cups_fill()' - Fill the input buffer.
//

static ssize_t				// O - Number of bytes or -1
cups_fill(cups_file_t *fp)		// I - CUPS file
{
  ssize_t		bytes;		// Number of bytes read
  int			status;		// Decompression status
  const unsigned char	*ptr,		// Pointer into buffer
			*end;		// End of buffer


  if (fp->ptr && fp->end)
    fp->bufpos += fp->end - fp->buf;

  while (!fp->ptr || fp->compressed)
  {
    // Check to see if we have read any data yet; if not, see if we have a compressed file...
    if (!fp->ptr)
    {
      // Reset the file position in case we are seeking...
      fp->compressed = false;

      // Read the first bytes in the file to determine if we have a gzip'd file...
      if ((bytes = cups_read(fp, (char *)fp->buf, sizeof(fp->buf))) < 0)
      {
        // Can't read from file!
        fp->eof = true;

	return (-1);
      }

      if (bytes < 10 || fp->buf[0] != 0x1f || (fp->buf[1] & 255) != 0x8b || fp->buf[2] != 8 || (fp->buf[3] & 0xe0) != 0)
      {
        // Not a gzip'd file!
	fp->ptr = fp->buf;
	fp->end = fp->buf + bytes;

	return (bytes);
      }

      // Parse header junk: extra data, original name, and comment...
      ptr = (unsigned char *)fp->buf + 10;
      end = (unsigned char *)fp->buf + bytes;

      if (fp->buf[3] & 0x04)
      {
        // Skip extra data...
	if ((ptr + 2) > end)
	{
	  // Can't read from file!
          fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}

	bytes = ((unsigned char)ptr[1] << 8) | (unsigned char)ptr[0];
	ptr   += 2 + bytes;

	if (ptr > end)
	{
	  // Can't read from file!
          fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}
      }

      if (fp->buf[3] & 0x08)
      {
        // Skip original name data...
	while (ptr < end && *ptr)
          ptr ++;

	if (ptr < end)
	{
          ptr ++;
	}
	else
	{
	  // Can't read from file!
          fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}
      }

      if (fp->buf[3] & 0x10)
      {
        // Skip comment data...
	while (ptr < end && *ptr)
          ptr ++;

	if (ptr < end)
	{
          ptr ++;
	}
	else
	{
	  // Can't read from file!
          fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}
      }

      if (fp->buf[3] & 0x02)
      {
        // Skip header CRC data...
	ptr += 2;

	if (ptr > end)
	{
	  // Can't read from file!
          fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}
      }

      // Copy the flate-compressed data to the compression buffer...
      if ((bytes = end - ptr) > 0)
        memcpy(fp->cbuf, ptr, (size_t)bytes);

      // Setup the decompressor data...
      fp->stream.zalloc    = (alloc_func)0;
      fp->stream.zfree     = (free_func)0;
      fp->stream.opaque    = (voidpf)0;
      fp->stream.next_in   = (Bytef *)fp->cbuf;
      fp->stream.next_out  = NULL;
      fp->stream.avail_in  = (uInt)bytes;
      fp->stream.avail_out = 0;
      fp->crc              = crc32(0L, Z_NULL, 0);

      if (inflateInit2(&(fp->stream), -15) != Z_OK)
      {
        fp->eof = true;
        errno   = EIO;

	return (-1);
      }

      fp->compressed = true;
    }

    if (fp->compressed)
    {
      // If we have reached end-of-file, return immediately...
      if (fp->eof)
	return (0);

      // Fill the decompression buffer as needed...
      if (fp->stream.avail_in == 0)
      {
	if ((bytes = cups_read(fp, (char *)fp->cbuf, sizeof(fp->cbuf))) <= 0)
	{
	  fp->eof = true;

          return (bytes);
	}

	fp->stream.next_in  = fp->cbuf;
	fp->stream.avail_in = (uInt)bytes;
      }

      // Decompress data from the buffer...
      fp->stream.next_out  = (Bytef *)fp->buf;
      fp->stream.avail_out = sizeof(fp->buf);

      status = inflate(&(fp->stream), Z_NO_FLUSH);

      if (fp->stream.next_out > (Bytef *)fp->buf)
        fp->crc = crc32(fp->crc, (Bytef *)fp->buf, (uInt)(fp->stream.next_out - (Bytef *)fp->buf));

      if (status == Z_STREAM_END)
      {
        // Read the CRC and length...
	unsigned char	trailer[8];	// Trailer bytes
	uLong		tcrc;		// Trailer CRC
	ssize_t		tbytes = 0;	// Number of bytes

	if (fp->stream.avail_in > 0)
	{
	  if (fp->stream.avail_in > sizeof(trailer))
	    tbytes = (ssize_t)sizeof(trailer);
	  else
	    tbytes = (ssize_t)fp->stream.avail_in;

	  memcpy(trailer, fp->stream.next_in, (size_t)tbytes);
	  fp->stream.next_in  += tbytes;
	  fp->stream.avail_in -= (size_t)tbytes;
	}

        if (tbytes < (ssize_t)sizeof(trailer))
	{
	  if (read(fp->fd, trailer + tbytes, sizeof(trailer) - (size_t)tbytes) < ((ssize_t)sizeof(trailer) - tbytes))
	  {
	    // Can't get it, so mark end-of-file...
	    fp->eof = true;
	    errno   = EIO;

	    return (-1);
	  }
	}

	tcrc = ((((((uLong)trailer[3] << 8) | (uLong)trailer[2]) << 8) | (uLong)trailer[1]) << 8) | (uLong)trailer[0];

	if (tcrc != fp->crc)
	{
	  // Bad CRC, mark end-of-file...
	  fp->eof = true;
	  errno   = EIO;

	  return (-1);
	}

        // Otherwise, reset the compressed flag so that we re-read the file header...
        inflateEnd(&fp->stream);

	fp->compressed = false;
      }
      else if (status < Z_OK)
      {
        fp->eof = true;
        errno   = EIO;

	return (-1);
      }

      bytes = (ssize_t)sizeof(fp->buf) - (ssize_t)fp->stream.avail_out;

      // Return the decompressed data...
      fp->ptr = fp->buf;
      fp->end = fp->buf + bytes;

      if (bytes)
	return (bytes);
    }
  }

  // Read a buffer's full of data...
  if ((bytes = cups_read(fp, fp->buf, sizeof(fp->buf))) <= 0)
  {
    // Can't read from file!
    fp->eof = true;
    fp->ptr = fp->buf;
    fp->end = fp->buf;
  }
  else
  {
    // Return the bytes we read...
    fp->eof = false;
    fp->ptr = fp->buf;
    fp->end = fp->buf + bytes;
  }

  return (bytes);
}


//
// 'cups_open()' - Safely open a file for writing.
//
// We don't allow appending to directories or files that are hard-linked or
// symlinked.
//

static int				// O - File descriptor or -1 otherwise
cups_open(const char *filename,		// I - Filename
          int        oflag,		// I - Open flags
	  int        mode)		// I - Open permissions
{
  int		fd;			// File descriptor
  struct stat	fileinfo;		// File information
#ifndef _WIN32
  struct stat	linkinfo;		// Link information
#endif // !_WIN32


  // Open the file...
  if ((fd = open(filename, oflag, mode)) < 0)
    return (-1);

  // Then verify that the file descriptor doesn't point to a directory or hard-linked file.
  if (fstat(fd, &fileinfo))
  {
    close(fd);
    return (-1);
  }

  if (fileinfo.st_nlink != 1)
  {
    close(fd);
    errno = EPERM;
    return (-1);
  }

#ifdef _WIN32
  if (fileinfo.st_mode & _S_IFDIR)
#else
  if (S_ISDIR(fileinfo.st_mode))
#endif // _WIN32
  {
    close(fd);
    errno = EISDIR;
    return (-1);
  }

#ifndef _WIN32
  // Then use lstat to determine whether the filename is a symlink...
  if (lstat(filename, &linkinfo))
  {
    close(fd);
    return (-1);
  }

  if (S_ISLNK(linkinfo.st_mode) ||
      fileinfo.st_dev != linkinfo.st_dev ||
      fileinfo.st_ino != linkinfo.st_ino ||
#ifdef HAVE_ST_GEN
      fileinfo.st_gen != linkinfo.st_gen ||
#endif // HAVE_ST_GEN
      fileinfo.st_nlink != linkinfo.st_nlink ||
      fileinfo.st_mode != linkinfo.st_mode)
  {
    // Yes, don't allow!
    close(fd);
    errno = EPERM;
    return (-1);
  }
#endif // !_WIN32

  return (fd);
}


//
// 'cups_read()' - Read from a file descriptor.
//

static ssize_t				// O - Number of bytes read or -1
cups_read(cups_file_t *fp,		// I - CUPS file
          char        *buf,		// I - Buffer
	  size_t      bytes)		// I - Number bytes
{
  ssize_t	total;			// Total bytes read


  // Loop until we read at least 0 bytes...
  for (;;)
  {
#ifdef _WIN32
    if (fp->mode == 's')
      total = (ssize_t)recv(fp->fd, buf, (unsigned)bytes, 0);
    else
      total = (ssize_t)read(fp->fd, buf, (unsigned)bytes);
#else
    if (fp->mode == 's')
      total = recv(fp->fd, buf, bytes, 0);
    else
      total = read(fp->fd, buf, bytes);
#endif // _WIN32

    if (total >= 0)
      break;

    // Reads can be interrupted by signals and unavailable resources...
    if (errno == EAGAIN || errno == EINTR)
      continue;
    else
      return (-1);
  }

  // Return the total number of bytes read...
  return (total);
}


//
// 'cups_write()' - Write to a file descriptor.
//

static bool				// O - `true` on success, `false` on error
cups_write(cups_file_t *fp,		// I - CUPS file
           const char  *buf,		// I - Buffer
	   size_t      bytes)		// I - Number bytes
{
  ssize_t	count;			// Count this time


  // Loop until all bytes are written...
  while (bytes > 0)
  {
#ifdef _WIN32
    if (fp->mode == 's')
      count = (ssize_t)send(fp->fd, buf, (unsigned)bytes, 0);
    else
      count = (ssize_t)write(fp->fd, buf, (unsigned)bytes);
#else
    if (fp->mode == 's')
      count = send(fp->fd, buf, bytes, 0);
    else
      count = write(fp->fd, buf, bytes);
#endif // _WIN32

    if (count < 0)
    {
      // Writes can be interrupted by signals and unavailable resources...
      if (errno == EAGAIN || errno == EINTR)
        continue;
      else
        return (false);
    }

    // Update the counts for the last write call...
    bytes -= (size_t)count;
    buf   += count;
  }

  // Return the total number of bytes written...
  return (true);
}
