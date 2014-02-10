
//          Copyright Hannes Domani 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file ../LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <windows.h>
#include <stdio.h>


#define DWST_DLL  "dwarfstack.dll"
#define DWST_FUNC "dwstExceptionDialog"


typedef HMODULE WINAPI func_LoadLibraryA( LPCSTR lpFileName );
typedef void *WINAPI func_GetProcAddress( HMODULE hModule,LPCSTR lpProcName );
typedef void func_dwstExceptionDialog( const char *extraInfo );

typedef struct
{
  func_LoadLibraryA *fLoadLibraryA;
  func_GetProcAddress *fGetProcAddress;
  char texts[2];
}
remoteData;

enum {
  REMOTE_OK,
  REMOTE_NO_DLL,
  REMOTE_NO_FUNC,
};

static DWORD WINAPI remoteCall( remoteData *data )
{
  const char *dllName = data->texts;
  const char *funcName = dllName;
  while( *funcName ) funcName++;
  funcName++;

  HMODULE mod = data->fLoadLibraryA( dllName );
  if( !mod ) return( REMOTE_NO_DLL );

  func_dwstExceptionDialog *dwstExceptionDialog =
    data->fGetProcAddress( mod,funcName );
  if( !dwstExceptionDialog ) return( REMOTE_NO_FUNC );

  dwstExceptionDialog( NULL );

  return( REMOTE_OK );
}

static void inject( HANDLE process,const char *dll,const char *func )
{
  size_t funcSize = (size_t)&inject - (size_t)&remoteCall;
  size_t fullSize = funcSize + sizeof(remoteData) +
    strlen( dll ) + strlen( func );

  char *fullData = malloc( fullSize );
  memcpy( fullData,&remoteCall,funcSize );
  remoteData *data = (remoteData*)( fullData+funcSize );

  HMODULE kernel32 = GetModuleHandle( "kernel32.dll" );
  data->fLoadLibraryA =
    (func_LoadLibraryA*)GetProcAddress( kernel32,"LoadLibraryA" );
  data->fGetProcAddress =
    (func_GetProcAddress*)GetProcAddress( kernel32,"GetProcAddress" );

  char *texts = data->texts;
  strcpy( texts,dll );
  texts += strlen( texts ) + 1;
  strcpy( texts,func );

  char *fullDataRemote = VirtualAllocEx( process,NULL,
      fullSize,MEM_COMMIT,PAGE_EXECUTE_READWRITE );
  if( !fullDataRemote )
  {
    printf( "can't allocate memory for remote process\n" );
    free( fullData );
    return;
  }
  WriteProcessMemory( process,fullDataRemote,fullData,fullSize,NULL );
  free( fullData );

  HANDLE thread = CreateRemoteThread( process,NULL,0,
      (LPTHREAD_START_ROUTINE)fullDataRemote,fullDataRemote+funcSize,0,NULL );
  if( !thread )
  {
    printf( "can't create thread\n" );
    VirtualFreeEx( process,fullDataRemote,fullSize,MEM_RELEASE );
    return;
  }

  WaitForSingleObject( thread,INFINITE );

  DWORD exitCode;
  if( GetExitCodeThread(thread,&exitCode) )
  {
    switch( exitCode )
    {
      case REMOTE_NO_DLL:
        printf( "can't find library '%s'\n",dll );
        break;

      case REMOTE_NO_FUNC:
        printf( "can't find function '%s'\n",func );
        break;
    }
  }

  CloseHandle( thread );

  VirtualFreeEx( process,fullDataRemote,fullSize,MEM_RELEASE );
}


static int isWrongArch( HANDLE process )
{
  BOOL remoteWow64,meWow64;
  IsWow64Process( process,&remoteWow64 );
  IsWow64Process( GetCurrentProcess(),&meWow64 );
  return( remoteWow64!=meWow64 );
}

int main( void )
{
  char *cmdLine = GetCommandLineA();
  char *args;
  if( cmdLine[0]=='"' && (args=strchr(cmdLine+1,'"')) )
    args++;
  else
    args = strchr( cmdLine,' ' );
  if( !args || !args[0] )
  {
    printf( "missing argument: application name or process id\n" );
    return( 1 );
  }
  while( args[0]==' ' ) args++;

  char dllPath[MAX_PATH];
  GetModuleFileNameA( NULL,dllPath,MAX_PATH );
  char *dirEnd = strrchr( dllPath,'\\' );
  if( dirEnd ) dirEnd++;
  else dirEnd = dllPath;
  strcpy( dirEnd,DWST_DLL );

  STARTUPINFO si = {0};
  PROCESS_INFORMATION pi = {0};
  si.cb = sizeof(STARTUPINFO);
  BOOL result = CreateProcess( NULL,args,NULL,NULL,FALSE,
      CREATE_SUSPENDED,NULL,NULL,&si,&pi );
  if( !result )
  {
    int processId = atoi( args );

    HANDLE process = NULL;
    if( processId )
      process = OpenProcess( PROCESS_ALL_ACCESS,FALSE,processId );
    if( !process )
    {
      printf( "can't create process for '%s'\n",args );
      return( 1 );
    }

    if( isWrongArch(process) )
      printf( "process %s is of a different architecture\n",args );
    else
      inject( process,dllPath,DWST_FUNC );

    CloseHandle( process );

    return( 0 );
  }

  if( isWrongArch(pi.hProcess) )
  {
    printf( "application '%s' is of a different architecture\n",args );

    TerminateProcess( pi.hProcess,1 );
  }
  else
  {
    inject( pi.hProcess,dllPath,DWST_FUNC );

    ResumeThread( pi.hThread );
  }

  CloseHandle( pi.hThread );
  CloseHandle( pi.hProcess );

  return( 0 );
}
