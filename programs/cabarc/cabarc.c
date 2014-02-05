/*
 * Tool to manipulate cabinet files
 *
 * Copyright 2011 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#include "fci.h"
#include "fdi.h"

#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cabarc);

/* from msvcrt */
#ifndef _O_RDONLY
#define _O_RDONLY      0
#define _O_WRONLY      1
#define _O_RDWR        2
#define _O_APPEND      0x0008
#define _O_RANDOM      0x0010
#define _O_SEQUENTIAL  0x0020
#define _O_TEMPORARY   0x0040
#define _O_NOINHERIT   0x0080
#define _O_CREAT       0x0100
#define _O_TRUNC       0x0200
#define _O_EXCL        0x0400
#define _O_SHORT_LIVED 0x1000
#define _O_TEXT        0x4000
#define _O_BINARY      0x8000
#endif

#ifndef _O_ACCMODE
#define _O_ACCMODE     (_O_RDONLY|_O_WRONLY|_O_RDWR)
#endif

#ifndef _SH_COMPAT
#define _SH_COMPAT     0x00
#define _SH_DENYRW     0x10
#define _SH_DENYWR     0x20
#define _SH_DENYRD     0x30
#define _SH_DENYNO     0x40
#endif

#ifndef _A_RDONLY
#define _A_RDONLY      0x01
#define _A_HIDDEN      0x02
#define _A_SYSTEM      0x04
#define _A_ARCH        0x20
#endif

/* command-line options */
static int opt_cabinet_size = CB_MAX_DISK;
static int opt_cabinet_id;
static int opt_compression = tcompTYPE_MSZIP;
static int opt_recurse;
static int opt_preserve_paths;
static int opt_reserve_space;
static int opt_verbose;
static char *opt_cab_file;
static WCHAR *opt_dest_dir;
static WCHAR **opt_files;

static void * CDECL cab_alloc( ULONG size )
{
    return HeapAlloc( GetProcessHeap(), 0, size );
}

static void CDECL cab_free( void *ptr )
{
    HeapFree( GetProcessHeap(), 0, ptr );
}

static WCHAR *strdupAtoW( UINT cp, const char *str )
{
    WCHAR *ret = NULL;
    if (str)
    {
        DWORD len = MultiByteToWideChar( cp, 0, str, -1, NULL, 0 );
        if ((ret = cab_alloc( len * sizeof(WCHAR) )))
            MultiByteToWideChar( cp, 0, str, -1, ret, len );
    }
    return ret;
}

static char *strdupWtoA( UINT cp, const WCHAR *str )
{
    char *ret = NULL;
    if (str)
    {
        DWORD len = WideCharToMultiByte( cp, 0, str, -1, NULL, 0, NULL, NULL );
        if ((ret = cab_alloc( len )))
            WideCharToMultiByte( cp, 0, str, -1, ret, len, NULL, NULL );
    }
    return ret;
}

/* format a cabinet name by replacing the '*' wildcard by the cabinet id */
static BOOL format_cab_name( char *dest, int id, const char *name )
{
    const char *num = strchr( name, '*' );
    int len;

    if (!num)
    {
        if (id == 1)
        {
            strcpy( dest, name );
            return TRUE;
        }
        WINE_MESSAGE( "cabarc: Cabinet name must contain a '*' character\n" );
        return FALSE;
    }
    len = num - name;
    memcpy( dest, name, len );
    len += sprintf( dest + len, "%u", id );
    lstrcpynA( dest + len, num + 1, CB_MAX_CABINET_NAME - len );
    return TRUE;
}

static int CDECL fci_file_placed( CCAB *cab, char *file, LONG size, BOOL continuation, void *ptr )
{
    if (!continuation && opt_verbose) printf( "adding %s\n", file );
    return 0;
}

static INT_PTR CDECL fci_open( char *file, int oflag, int pmode, int *err, void *ptr )
{
    DWORD creation = 0, sharing = 0;
    int ioflag = 0;
    HANDLE handle;

    switch (oflag & _O_ACCMODE)
    {
    case _O_RDONLY: ioflag |= GENERIC_READ; break;
    case _O_WRONLY: ioflag |= GENERIC_WRITE; break;
    case _O_RDWR:   ioflag |= GENERIC_READ | GENERIC_WRITE; break;
    }

    if (oflag & _O_CREAT)
    {
        if (oflag & _O_EXCL) creation = CREATE_NEW;
        else if (oflag & _O_TRUNC) creation = CREATE_ALWAYS;
        else creation = OPEN_ALWAYS;
    }
    else
    {
        if (oflag & _O_TRUNC) creation = TRUNCATE_EXISTING;
        else creation = OPEN_EXISTING;
    }

    switch (pmode & 0x70)
    {
    case _SH_DENYRW: sharing = 0; break;
    case _SH_DENYWR: sharing = FILE_SHARE_READ; break;
    case _SH_DENYRD: sharing = FILE_SHARE_WRITE; break;
    default:         sharing = FILE_SHARE_READ | FILE_SHARE_WRITE; break;
    }

    handle = CreateFileA( file, ioflag, sharing, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL );
    if (handle == INVALID_HANDLE_VALUE) *err = GetLastError();
    return (INT_PTR)handle;
}

static UINT CDECL fci_read( INT_PTR hf, void *pv, UINT cb, int *err, void *ptr )
{
    DWORD num_read;

    if (!ReadFile( (HANDLE)hf, pv, cb, &num_read, NULL ))
    {
        *err = GetLastError();
        return -1;
    }
    return num_read;
}

static UINT CDECL fci_write( INT_PTR hf, void *pv, UINT cb, int *err, void *ptr )
{
    DWORD written;

    if (!WriteFile( (HANDLE) hf, pv, cb, &written, NULL ))
    {
        *err = GetLastError();
        return -1;
    }
    return written;
}

static int CDECL fci_close( INT_PTR hf, int *err, void *ptr )
{
    if (!CloseHandle( (HANDLE)hf ))
    {
        *err = GetLastError();
        return -1;
    }
    return 0;
}

static LONG CDECL fci_lseek( INT_PTR hf, LONG dist, int seektype, int *err, void *ptr )
{
    DWORD ret;

    ret = SetFilePointer( (HANDLE)hf, dist, NULL, seektype );
    if (ret == INVALID_SET_FILE_POINTER && GetLastError())
    {
        *err = GetLastError();
        return -1;
    }
    return ret;
}

static int CDECL fci_delete( char *file, int *err, void *ptr )
{
    if (!DeleteFileA( file ))
    {
        *err = GetLastError();
        return -1;
    }
    return 0;
}

static BOOL CDECL fci_get_temp( char *name, int size, void *ptr )
{
    char path[MAX_PATH];

    if (!GetTempPathA( MAX_PATH, path )) return FALSE;
    if (!GetTempFileNameA( path, "cab", 0, name )) return FALSE;
    DeleteFileA( name );
    return TRUE;
}

static BOOL CDECL fci_get_next_cab( CCAB *cab, ULONG prev_size, void *ptr )
{
    return format_cab_name( cab->szCab, cab->iCab + 1, opt_cab_file );
}

static LONG CDECL fci_status( UINT type, ULONG cb1, ULONG cb2, void *ptr )
{
    return 0;
}

static INT_PTR CDECL fci_get_open_info( char *name, USHORT *date, USHORT *time,
                                        USHORT *attribs, int *err, void *ptr )
{
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    WCHAR *p, *nameW = strdupAtoW( CP_UTF8, name );

    handle = CreateFileW( nameW, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL );
    if (handle == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        WINE_ERR( "failed to open %s: error %u\n", wine_dbgstr_w(nameW), *err );
        cab_free( nameW );
        return -1;
    }
    if (!GetFileInformationByHandle( handle, &info ))
    {
        *err = GetLastError();
        CloseHandle( handle );
        cab_free( nameW );
        return -1;
    }
    FileTimeToDosDateTime( &info.ftLastWriteTime, date, time );
    *attribs = info.dwFileAttributes & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH);
    for (p = nameW; *p; p++) if (*p >= 0x80) break;
    if (*p) *attribs |= _A_NAME_IS_UTF;
    cab_free( nameW );
    return (INT_PTR)handle;
}

static INT_PTR CDECL fdi_open( char *file, int oflag, int pmode )
{
    int err;
    return fci_open( file, oflag, pmode, &err, NULL );
}

static UINT CDECL fdi_read( INT_PTR hf, void *pv, UINT cb )
{
    int err;
    return fci_read( hf, pv, cb, &err, NULL );
}

static UINT CDECL fdi_write( INT_PTR hf, void *pv, UINT cb )
{
    int err;
    return fci_write( hf, pv, cb, &err, NULL );
}

static int CDECL fdi_close( INT_PTR hf )
{
    int err;
    return fci_close( hf, &err, NULL );
}

static LONG CDECL fdi_lseek( INT_PTR hf, LONG dist, int whence )
{
    int err;
    return fci_lseek( hf, dist, whence, &err, NULL );
}


/* create directories leading to a given file */
static void create_directories( const WCHAR *name )
{
    WCHAR *path, *p;

    /* create the directory/directories */
    path = cab_alloc( (strlenW(name) + 1) * sizeof(WCHAR) );
    strcpyW(path, name);

    p = strchrW(path, '\\');
    while (p != NULL)
    {
        *p = 0;
        if (!CreateDirectoryW( path, NULL ))
            WINE_TRACE("Couldn't create directory %s - error: %d\n", wine_dbgstr_w(path), GetLastError());
        *p = '\\';
        p = strchrW(p+1, '\\');
    }
    cab_free( path );
}

/* check if file name matches against one of the files specification */
static BOOL match_files( const WCHAR *name )
{
    int i;

    if (!*opt_files) return TRUE;
    for (i = 0; opt_files[i]; i++)
    {
        unsigned int len = strlenW( opt_files[i] );
        /* FIXME: do smarter matching, and wildcards */
        if (!len) continue;
        if (strncmpiW( name, opt_files[i], len )) continue;
        if (opt_files[i][len - 1] == '\\' || !name[len] || name[len] == '\\') return TRUE;
    }
    return FALSE;
}

static INT_PTR CDECL list_notify( FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin )
{
    WCHAR *nameW;

    switch (fdint)
    {
    case fdintCABINET_INFO:
        return 0;
    case fdintCOPY_FILE:
        nameW = strdupAtoW( (pfdin->attribs & _A_NAME_IS_UTF) ? CP_UTF8 : CP_ACP, pfdin->psz1 );
        if (match_files( nameW ))
        {
            char *nameU = strdupWtoA( CP_UNIXCP, nameW );
            if (opt_verbose)
            {
                char attrs[] = "rxash";
                if (!(pfdin->attribs & _A_RDONLY)) attrs[0] = '-';
                if (!(pfdin->attribs & _A_EXEC))   attrs[1] = '-';
                if (!(pfdin->attribs & _A_ARCH))   attrs[2] = '-';
                if (!(pfdin->attribs & _A_SYSTEM)) attrs[3] = '-';
                if (!(pfdin->attribs & _A_HIDDEN)) attrs[4] = '-';
                printf( " %s %9u  %04u/%02u/%02u %02u:%02u:%02u  ", attrs, pfdin->cb,
                        (pfdin->date >> 9) + 1980, (pfdin->date >> 5) & 0x0f, pfdin->date & 0x1f,
                        pfdin->time >> 11, (pfdin->time >> 5) & 0x3f, (pfdin->time & 0x1f) * 2 );
            }
            printf( "%s\n", nameU );
            cab_free( nameU );
        }
        cab_free( nameW );
        return 0;
    default:
        WINE_FIXME( "Unexpected notification type %d.\n", fdint );
        return 0;
    }
}

static int list_cabinet( char *cab_dir )
{
    ERF erf;
    int ret = 0;
    HFDI fdi = FDICreate( cab_alloc, cab_free, fdi_open, fdi_read,
                          fdi_write, fdi_close, fdi_lseek, cpuUNKNOWN, &erf );

    if (!FDICopy( fdi, opt_cab_file, cab_dir, 0, list_notify, NULL, NULL )) ret = GetLastError();
    FDIDestroy( fdi );
    return ret;
}

static INT_PTR CDECL extract_notify( FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin )
{
    WCHAR *file, *nameW, *path = NULL;
    INT_PTR ret;

    switch (fdint)
    {
    case fdintCABINET_INFO:
        return 0;

    case fdintCOPY_FILE:
        nameW = strdupAtoW( (pfdin->attribs & _A_NAME_IS_UTF) ? CP_UTF8 : CP_ACP, pfdin->psz1 );
        if (opt_preserve_paths)
        {
            file = nameW;
            while (*file == '\\') file++;  /* remove leading backslashes */
        }
        else
        {
            if ((file = strrchrW( nameW, '\\' ))) file++;
            else file = nameW;
        }

        if (opt_dest_dir)
        {
            path = cab_alloc( (strlenW(opt_dest_dir) + strlenW(file) + 1) * sizeof(WCHAR) );
            strcpyW( path, opt_dest_dir );
            strcatW( path, file );
        }
        else path = file;

        if (match_files( file ))
        {
            if (opt_verbose)
            {
                char *nameU = strdupWtoA( CP_UNIXCP, path );
                printf( "extracting %s\n", nameU );
                cab_free( nameU );
            }
            create_directories( path );
            /* FIXME: check for existing file and overwrite mode */
            ret = (INT_PTR)CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        }
        else ret = 0;

        cab_free( nameW );
        if (path != file) cab_free( path );
        return ret;

    case fdintCLOSE_FILE_INFO:
        CloseHandle( (HANDLE)pfdin->hf );
        return 0;

    case fdintNEXT_CABINET:
        WINE_TRACE("Next cab: status %u, path '%s', file '%s'\n", pfdin->fdie, pfdin->psz3, pfdin->psz1);
        return pfdin->fdie == FDIERROR_NONE ? 0 : -1;

    case fdintENUMERATE:
        return 0;

    default:
        WINE_FIXME( "Unexpected notification type %d.\n", fdint );
        return 0;
    }
}

static int extract_cabinet( char *cab_dir )
{
    ERF erf;
    int ret = 0;
    HFDI fdi = FDICreate( cab_alloc, cab_free, fdi_open, fdi_read,
                          fdi_write, fdi_close, fdi_lseek, cpuUNKNOWN, &erf );

    if (!FDICopy( fdi, opt_cab_file, cab_dir, 0, extract_notify, NULL, NULL ))
    {
        ret = GetLastError();
        WINE_WARN("FDICopy() failed: code %u\n", ret);
    }
    FDIDestroy( fdi );
    return ret;
}

static BOOL add_file( HFCI fci, WCHAR *name )
{
    BOOL ret;
    char *filename, *path = strdupWtoA( CP_UTF8, name );

    if (!opt_preserve_paths)
    {
        if ((filename = strrchr( path, '\\' ))) filename++;
        else filename = path;
    }
    else
    {
        filename = path;
        while (*filename == '\\') filename++;  /* remove leading backslashes */
    }
    ret = FCIAddFile( fci, path, filename, FALSE,
                      fci_get_next_cab, fci_status, fci_get_open_info, opt_compression );
    cab_free( path );
    return ret;
}

static BOOL add_directory( HFCI fci, WCHAR *dir )
{
    static const WCHAR wildcardW[] = {'*',0};
    WCHAR *p, *buffer;
    HANDLE handle;
    WIN32_FIND_DATAW data;
    BOOL ret = TRUE;

    if (!(buffer = cab_alloc( (strlenW(dir) + MAX_PATH + 2) * sizeof(WCHAR) ))) return FALSE;
    strcpyW( buffer, dir );
    p = buffer + strlenW( buffer );
    if (p > buffer && p[-1] != '\\') *p++ = '\\';
    strcpyW( p, wildcardW );

    if ((handle = FindFirstFileW( buffer, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (data.cFileName[0] == '.' && !data.cFileName[1]) continue;
            if (data.cFileName[0] == '.' && data.cFileName[1] == '.' && !data.cFileName[2]) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            strcpyW( p, data.cFileName );
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ret = add_directory( fci, buffer );
            else
                ret = add_file( fci, buffer );
            if (!ret) break;
        } while (FindNextFileW( handle, &data ));
        FindClose( handle );
    }
    cab_free( buffer );
    return TRUE;
}

static BOOL add_file_or_directory( HFCI fci, WCHAR *name )
{
    DWORD attr = GetFileAttributesW( name );

    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        WINE_MESSAGE( "cannot open %s\n", wine_dbgstr_w(name) );
        return FALSE;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        if (opt_recurse) return add_directory( fci, name );
        WINE_MESSAGE( "cabarc: Cannot add %s, it's a directory (use -r for recursive add)\n",
                      wine_dbgstr_w(name) );
        return FALSE;
    }
    return add_file( fci, name );
}

static int new_cabinet( char *cab_dir )
{
    static const WCHAR plusW[] = {'+',0};
    WCHAR **file;
    ERF erf;
    BOOL ret = FALSE;
    HFCI fci;
    CCAB cab;

    cab.cb                = opt_cabinet_size;
    cab.cbFolderThresh    = CB_MAX_DISK;
    cab.cbReserveCFHeader = opt_reserve_space;
    cab.cbReserveCFFolder = 0;
    cab.cbReserveCFData   = 0;
    cab.iCab              = 0;
    cab.iDisk             = 0;
    cab.setID             = opt_cabinet_id;
    cab.szDisk[0]         = 0;

    strcpy( cab.szCabPath, cab_dir );
    strcat( cab.szCabPath, "\\" );
    format_cab_name( cab.szCab, 1, opt_cab_file );

    fci = FCICreate( &erf, fci_file_placed, cab_alloc, cab_free,fci_open, fci_read,
                     fci_write, fci_close, fci_lseek, fci_delete, fci_get_temp, &cab, NULL );

    for (file = opt_files; *file; file++)
    {
        if (!strcmpW( *file, plusW ))
            FCIFlushFolder( fci, fci_get_next_cab, fci_status );
        else
            if (!(ret = add_file_or_directory( fci, *file ))) break;
    }

    if (ret)
    {
        if (!(ret = FCIFlushCabinet( fci, FALSE, fci_get_next_cab, fci_status )))
            WINE_MESSAGE( "cabarc: Failed to create cabinet %s\n", wine_dbgstr_a(opt_cab_file) );
    }
    FCIDestroy( fci );
    return !ret;
}

static void usage( void )
{
    WINE_MESSAGE(
        "Usage: cabarc [options] command file.cab [files...] [dest_dir\\]\n"
        "\nCommands:\n"
        "   L   List the contents of the cabinet\n"
        "   N   Create a new cabinet\n"
        "   X   Extract files from the cabinet into dest_dir\n"
        "\nOptions:\n"
        "  -d size  Set maximum disk size\n"
        "  -h       Display this help\n"
        "  -i id    Set cabinet id\n"
        "  -m type  Set compression type (mszip|none)\n"
        "  -p       Preserve directory names\n"
        "  -r       Recurse into directories\n"
        "  -s size  Reserve space in the cabinet header\n"
        "  -v       More verbose output\n" );
}

int wmain( int argc, WCHAR *argv[] )
{
    static const WCHAR noneW[] = {'n','o','n','e',0};
    static const WCHAR mszipW[] = {'m','s','z','i','p',0};

    WCHAR *p, *command;
    char buffer[MAX_PATH];
    char filename[MAX_PATH];
    char *cab_file, *file_part;
    int i;

    while (argv[1] && argv[1][0] == '-')
    {
        switch (argv[1][1])
        {
        case 'd':
            argv++; argc--;
            opt_cabinet_size = atoiW( argv[1] );
            if (opt_cabinet_size < 50000)
            {
                WINE_MESSAGE( "cabarc: Cabinet size must be at least 50000\n" );
                return 1;
            }
            break;
        case 'h':
            usage();
            return 0;
        case 'i':
            argv++; argc--;
            opt_cabinet_id = atoiW( argv[1] );
            break;
        case 'm':
            argv++; argc--;
            if (!strcmpiW( argv[1], noneW )) opt_compression = tcompTYPE_NONE;
            else if (!strcmpiW( argv[1], mszipW )) opt_compression = tcompTYPE_MSZIP;
            else
            {
                char *arg = strdupWtoA( CP_ACP, argv[1] );
                WINE_MESSAGE( "cabarc: Unknown compression type '%s'\n", arg);
                return 1;
            }
            break;
        case 'p':
            opt_preserve_paths = 1;
            break;
        case 'r':
            opt_recurse = 1;
            break;
        case 's':
            argv++; argc--;
            opt_reserve_space = atoiW( argv[1] );
            break;
        case 'v':
            opt_verbose++;
            break;
        default:
            usage();
            return 1;
        }
        argv++; argc--;
    }

    command = argv[1];
    if (argc < 3 || !command[0] || command[1])
    {
        usage();
        return 1;
    }
    cab_file = strdupWtoA( CP_ACP, argv[2] );
    argv += 2;
    argc -= 2;

    if (!GetFullPathNameA( cab_file, MAX_PATH, buffer, &file_part ) || !file_part)
    {
        WINE_ERR( "cannot get full name for %s\n", wine_dbgstr_a( cab_file ));
        return 1;
    }
    strcpy(filename, file_part);
    file_part[0] = 0;

    /* map slash to backslash in all file arguments */
    for (i = 1; i < argc; i++)
        for (p = argv[i]; *p; p++)
            if (*p == '/') *p = '\\';
    opt_files = argv + 1;
    opt_cab_file = filename;

    switch (*command)
    {
    case 'l':
    case 'L':
        return list_cabinet( buffer );
    case 'n':
    case 'N':
        return new_cabinet( buffer );
    case 'x':
    case 'X':
        if (argc > 1)  /* check for destination dir as last argument */
        {
            WCHAR *last = argv[argc - 1];
            if (last[0] && last[strlenW(last) - 1] == '\\')
            {
                opt_dest_dir = last;
                argv[--argc] = NULL;
            }
        }
        WINE_TRACE("Extracting file(s) from cabinet %s\n", wine_dbgstr_a(cab_file));
        return extract_cabinet( buffer );
    default:
        usage();
        return 1;
    }
}
