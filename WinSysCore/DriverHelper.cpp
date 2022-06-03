#include "pch.h"
#include "DriverHelper.h"
#include "SecurityHelper.h"

HANDLE DriverHelper::_hDevice;

#define IRP_MJ_MAXIMUM_FUNCTION         0x1b

bool DriverHelper::LoadDriver(bool load) {
	if (_hDevice) {
		::CloseHandle(_hDevice);
		_hDevice = INVALID_HANDLE_VALUE;
	}
	wil::unique_schandle hScm(::OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
	if (!hScm)
		return false;

	wil::unique_schandle hService(::OpenService(hScm.get(), L"AntiRootkit", SERVICE_ALL_ACCESS));
	if (!hService)
		return false;

	SERVICE_STATUS status;
	bool success = true;
	DWORD targetState;
	::QueryServiceStatus(hService.get(), &status);
	if (load && status.dwCurrentState != (targetState = SERVICE_RUNNING))
		success = ::StartService(hService.get(), 0, nullptr);
	else if (!load && status.dwCurrentState != (targetState = SERVICE_STOPPED))
		success = ::ControlService(hService.get(), SERVICE_CONTROL_STOP, &status);
	else
		return true;

	if (!success)
		return false;

	for (int i = 0; i < 20; i++) {
		::QueryServiceStatus(hService.get(), &status);
		if (status.dwCurrentState == targetState)
			return true;
		::Sleep(200);
	}

	return false;
}

bool DriverHelper::InstallDriver(bool justCopy,void* pBuffer,DWORD size) {
	if (!SecurityHelper::IsRunningElevated())
		return false;
	
	WCHAR path[MAX_PATH];
	::GetSystemDirectory(path, MAX_PATH);
	::wcscat_s(path, L"\\Drivers\\AntiRootkit.sys");
	wil::unique_hfile hFile(::CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, nullptr));
	if (!hFile)
		return false;

	DWORD bytes = 0;
	::WriteFile(hFile.get(), pBuffer, size, &bytes, nullptr);
	if (bytes != size)
		return false;

	if (justCopy)
		return true;

	wil::unique_schandle hScm(::OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
	if (!hScm)
		return false;

	wil::unique_schandle hService(::CreateService(hScm.get(), L"AntiRootkit", nullptr, SERVICE_ALL_ACCESS, 
		SERVICE_KERNEL_DRIVER,
		SERVICE_AUTO_START, // driver is loaded automatically by the SCM when Windows subsystem is available
		SERVICE_ERROR_NORMAL, // the error is logged to the event log service
		path, // the full path to the executable to run for the service
		nullptr, nullptr, nullptr, nullptr, nullptr));
	if (ERROR_SERVICE_EXISTS == GetLastError()) {
		hService.reset(::OpenService(hScm.get(), L"AntiRootkit", SERVICE_ALL_ACCESS));
		if (!hService)
			return false;
		if (!DeleteService(hService.get()))
			return false;
		hService.reset(::CreateService(hScm.get(), L"AntiRootkit", nullptr, SERVICE_ALL_ACCESS,
			SERVICE_KERNEL_DRIVER,
			SERVICE_AUTO_START, // driver is loaded automatically by the SCM when Windows subsystem is available
			SERVICE_ERROR_NORMAL, // the error is logged to the event log service
			path, // the full path to the executable to run for the service
			nullptr, nullptr, nullptr, nullptr, nullptr));
	}
	auto success = hService != nullptr;
	return success;
}

bool DriverHelper::UpdateDriver(void* pBuffer,DWORD size) {
	if (!LoadDriver(false))
		return false;
	if (!InstallDriver(true,pBuffer,size))
		return false;
	if (!LoadDriver())
		return false;

	return true;
}

HANDLE DriverHelper::OpenHandle(void* pObject, ACCESS_MASK access) {
	if (!OpenDevice())
		return nullptr;

	OpenObjectData data;
	data.Access = access;
	data.Address = pObject;

	DWORD bytes;
	HANDLE hObject;
	return ::DeviceIoControl(_hDevice, IOCTL_ARK_OPEN_OBJECT, &data, sizeof(data), &hObject, sizeof(hObject), &bytes, nullptr) ? hObject : nullptr;
}

HANDLE DriverHelper::OpenProcess(DWORD pid, ACCESS_MASK access) {
	if (OpenDevice()) {
		OpenProcessThreadData data;
		data.AccessMask = access;
		data.Id = pid;
		HANDLE hProcess;
		DWORD bytes;

		return ::DeviceIoControl(_hDevice, IOCTL_ARK_OPEN_PROCESS, &data, sizeof(data),
			&hProcess, sizeof(hProcess), &bytes, nullptr) ? hProcess : nullptr;
	}

	return ::OpenProcess(access, FALSE, pid);
}

bool DriverHelper::OpenDevice() {
	if (!_hDevice||_hDevice==INVALID_HANDLE_VALUE) {
		_hDevice = ::CreateFile(L"\\\\.\\AntiRootkit", GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, 0, nullptr);
		if (_hDevice == INVALID_HANDLE_VALUE) {
			_hDevice = nullptr;
			return false;
		}
	}
	return true;
}

bool DriverHelper::IsDriverLoaded() {
	wil::unique_schandle hScm(::OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
	if (!hScm)
		return false;

	wil::unique_schandle hService(::OpenService(hScm.get(), L"AntiRootkit", SERVICE_QUERY_STATUS));
	if (!hService)
		return false;

	SERVICE_STATUS status;
	if (!::QueryServiceStatus(hService.get(), &status))
		return false;

	return status.dwCurrentState == SERVICE_RUNNING;
}

bool DriverHelper::CloseDevice() {
	if (_hDevice) {
		::CloseHandle(_hDevice);
		_hDevice = nullptr;
	}
	return true;
}

USHORT DriverHelper::GetVersion() {
	USHORT version = 0;
	if (!OpenDevice())
		return 0;

	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_VERSION, nullptr, 0, &version, sizeof(version), &bytes, nullptr);
	return version;
}


USHORT DriverHelper::GetCurrentVersion() {
	return DRIVER_CURRENT_VERSION;
}

HANDLE DriverHelper::DupHandle(HANDLE hObject, ULONG pid, ACCESS_MASK access, DWORD flags) {
	HANDLE hTarget = nullptr;
	if (OpenDevice()) {
		DupHandleData data;
		data.AccessMask = access;
		data.Handle = HandleToUlong(hObject);
		data.SourcePid = pid;
		data.Flags = flags;

		DWORD bytes;
		::DeviceIoControl(_hDevice, IOCTL_ARK_DUP_HANDLE, &data, sizeof(data),
			&hTarget, sizeof(hTarget), &bytes, nullptr);
	}
	if (!hTarget) {
		wil::unique_handle hProcess(OpenProcess(pid, PROCESS_DUP_HANDLE));
		if (hProcess)
			::DuplicateHandle(hObject, hProcess.get(), ::GetCurrentProcess(), &hTarget, access, FALSE, flags);
	}
	return hTarget;
}

HANDLE DriverHelper::OpenThread(DWORD tid, ACCESS_MASK access) {
	if (OpenDevice()) {
		OpenProcessThreadData data;
		data.AccessMask = access;
		data.Id = tid;
		HANDLE hThread;
		DWORD bytes;

		return ::DeviceIoControl(_hDevice, IOCTL_ARK_OPEN_THREAD, &data, sizeof(data),
			&hThread, sizeof(hThread), &bytes, nullptr) ? hThread : nullptr;
	}
	return ::OpenThread(access, FALSE, tid);
}


HANDLE DriverHelper::OpenKey(PCWSTR name,ACCESS_MASK access) {
	if (!OpenDevice())
		return nullptr;

	auto len = (ULONG)::wcslen(name);
	DWORD size = len * sizeof(WCHAR) + sizeof(KeyData);
	auto buffer = std::make_unique<BYTE[]>(size);
	if (!buffer)
		return nullptr;

	auto data = reinterpret_cast<KeyData*>(buffer.get());
	data->Access = access;
	data->Length = len;
	::wcscpy_s(data->Name, len + 1, name);

	DWORD bytes;
	HANDLE hObject = nullptr;
	::DeviceIoControl(_hDevice, IOCTL_ARK_OPEN_KEY, data, size, &hObject, sizeof(hObject), &bytes, nullptr);

	return hObject;
}

PULONG DriverHelper::GetShadowServiceTable(PULONG* pServiceDescriptor) {
	PULONG address = 0;
	if (!OpenDevice())
		return 0;

	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_SHADOW_SERVICE_TABLE, pServiceDescriptor, sizeof(pServiceDescriptor), 
		&address, sizeof(address), &bytes, nullptr);
	return address;
}

PVOID DriverHelper::GetSSDTApiAddress(ULONG number) {
	if (!OpenDevice())
		return nullptr;

	PVOID address = nullptr;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_SSDT_API_ADDR, &number, sizeof(number),
		&address, sizeof(address), &bytes, nullptr);
	return address;
}

PVOID DriverHelper::GetShadowSSDTApiAddress(ULONG number) {
	if (!OpenDevice())
		return nullptr;

	PVOID address = nullptr;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_SHADOW_SSDT_API_ADDR, &number, sizeof(number),
		&address, sizeof(address), &bytes, nullptr);
	return address;
}

bool DriverHelper::InitNtServiceTable(PULONG * pTable) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_INIT_NT_SERVICE_TABLE, pTable, sizeof(pTable),
		nullptr, 0, &bytes, nullptr);
	return true;
}

ULONG DriverHelper::GetServiceLimit(PULONG* pTable) {
	if (!OpenDevice())
		return 0;

	ULONG limit = 0;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_SERVICE_LIMIT, pTable, sizeof(pTable), 
		&limit, sizeof(limit),&bytes, nullptr);
	return limit;
}

ULONG DriverHelper::GetProcessNotifyCount(ProcessNotifyCountData *pData) {
	if (!OpenDevice())
		return 0;

	ULONG count = 0;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_PROCESS_NOTIFY_COUNT, pData, sizeof(ProcessNotifyCountData),
		&count, sizeof(count), &bytes, nullptr);
	return count;
}

ULONG DriverHelper::GetThreadNotifyCount(ThreadNotifyCountData* pData) {
	if (!OpenDevice())
		return 0;

	ULONG count = 0;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_THREAD_NOTIFY_COUNT, pData, sizeof(ThreadNotifyCountData),
		&count, sizeof(count), &bytes, nullptr);
	return count;
}

bool DriverHelper::EnumProcessNotify(NotifyInfo* pNotifyInfo, KernelCallbackInfo* pCallbackInfo) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	DWORD size = pCallbackInfo->Count * sizeof(void*) + sizeof(ULONG);
	::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_PROCESS_NOTIFY, pNotifyInfo, sizeof(NotifyInfo),
		pCallbackInfo, size, &bytes, nullptr);
	return true;
}

bool DriverHelper::EnumThreadNotify(NotifyInfo* pNotifyInfo, KernelCallbackInfo* pCallbackInfo) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	DWORD size = pCallbackInfo->Count * sizeof(void*) + sizeof(ULONG);
	::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_THREAD_NOTIFY, pNotifyInfo, sizeof(NotifyInfo),
		pCallbackInfo, size, &bytes, nullptr);
	return true;
}

bool DriverHelper::EnumImageLoadNotify(NotifyInfo* pNotifyInfo, KernelCallbackInfo* pCallbackInfo) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	DWORD size = pCallbackInfo->Count * sizeof(void*)+sizeof(ULONG);
	::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_IMAGELOAD_NOTIFY, pNotifyInfo, sizeof(NotifyInfo),
		pCallbackInfo, size, &bytes, nullptr);
	return true;
}

ULONG DriverHelper::GetImageNotifyCount(PULONG* pCount) {
	if (!OpenDevice())
		return false;

	ULONG count = 0;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_IMAGE_NOTIFY_COUNT, pCount, sizeof(pCount),
		&count, sizeof(count), &bytes, nullptr);
	return count;
}

ULONG DriverHelper::GetPiDDBCacheDataSize(ULONG_PTR Address) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	DWORD size = 0;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_PIDDBCACHE_DATA_SIZE, &Address, sizeof(ULONG_PTR),
		&size, sizeof(DWORD), &bytes, nullptr);
	return size;
}

bool DriverHelper::EnumPiDDBCacheTable(ULONG_PTR Address,PVOID buffer,ULONG size) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_PIDDBCACHE_TABLE, &Address, sizeof(ULONG_PTR),
		buffer, size, &bytes, nullptr);
	return true;
}

ULONG DriverHelper::GetUnloadedDriverCount(PULONG * pCount) {
	if (!OpenDevice())
		return false;

	ULONG count = 0;
	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_IMAGE_NOTIFY_COUNT, pCount, sizeof(pCount),
		&count, sizeof(count), &bytes, nullptr);
	return count;
}

bool DriverHelper::EnumUnloadedDrivers(UnloadedDriversInfo* pInfo,PVOID buffer,ULONG size) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_UNLOADED_DRIVERS, pInfo, sizeof(UnloadedDriversInfo),
		buffer, size, &bytes, nullptr);
	return true;
}

ULONG DriverHelper::GetUnloadedDriverDataSize(UnloadedDriversInfo* pInfo) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	DWORD size = 0;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_UNLOADED_DRIVERS_DATA_SIZE, pInfo, sizeof(UnloadedDriversInfo),
		&size, sizeof(DWORD), &bytes, nullptr);
	return size;
}

bool DriverHelper::EnumObCallbackNotify(KernelNotifyInfo* pNotifyInfo,ObCallbackInfo* pCallbackInfo,ULONG size) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	::DeviceIoControl(_hDevice,IOCTL_ARK_ENUM_OBJECT_CALLBACK_NOTIFY, pNotifyInfo, sizeof(NotifyInfo),
		pCallbackInfo, size, &bytes, nullptr);
	return true;
}

LONG DriverHelper::GetObCallbackCount(KernelNotifyInfo* pNotifyInfo) {
	if (!OpenDevice())
		return 0;

	DWORD bytes;
	LONG count = 0;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_OBJECT_CALLBACK_NOTIFY_COUNT, pNotifyInfo, sizeof(NotifyInfo),
		&count, sizeof(LONG), &bytes, nullptr);
	return count;
}

ULONG DriverHelper::GetCmCallbackCount(PULONG* pCount) {
	if (!OpenDevice())
		return 0;

	DWORD bytes;
	ULONG count = 0;
	::DeviceIoControl(_hDevice, IOCTL_ARK_GET_CM_CALLBACK_NOTIFY_COUNT, pCount, sizeof(pCount),
		&count, sizeof(ULONG), &bytes, nullptr);
	return count;
}

bool DriverHelper::EnumCmCallbackNotify(PVOID pHeadList, CmCallbackInfo* pCallbackInfo, ULONG size){
	if (!OpenDevice())
		return 0;

	DWORD bytes;
	return ::DeviceIoControl(_hDevice, IOCTL_ARK_ENUM_CM_CALLBACK_NOTIFY, &pHeadList, sizeof(pHeadList),
		pCallbackInfo, size, &bytes, nullptr);
}

bool DriverHelper::GetDriverObjectRoutines(PCWSTR name, PVOID pRoutines) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	LONG count = 0;
	DWORD len = ::wcslen(name) + 1;
	len = len * sizeof(WCHAR);

	return ::DeviceIoControl(_hDevice, IOCTL_ARK_GET_DRIVER_OBJECT_ROUTINES, (LPVOID)name, len,
		pRoutines, sizeof(void*) * IRP_MJ_MAXIMUM_FUNCTION, &bytes, nullptr);
}

bool DriverHelper::SetImageLoadNotify() {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	return ::DeviceIoControl(_hDevice, IOCTL_ARK_OPEN_INTERCEPT_DRIVER_LOAD,
		nullptr, 0, nullptr, 0, &bytes, nullptr);
}

bool DriverHelper::RemoveImageLoadNotify() {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	return ::DeviceIoControl(_hDevice, IOCTL_ARK_CLOSE_INTERCEPT_DRIVER_LOAD, 
		nullptr, 0, nullptr, 0, &bytes, nullptr);
}

bool DriverHelper::RemoveNotify(NotifyData* pData) {
	if (!OpenDevice())
		return false;

	DWORD bytes;
	return ::DeviceIoControl(_hDevice, IOCTL_ARK_REMOVE_KERNEL_NOTIFY, pData, sizeof(NotifyData),
		nullptr, 0, &bytes, nullptr);
}