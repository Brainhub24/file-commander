#include "cshell.h"
#include "filesystemhelperfunctions.h"
#include "settings/csettings.h"
#include "settings.h"
#include "compiler/compiler_warnings_control.h"
#include "assert/advanced_assert.h"
#include "system/win_utils.hpp"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <thread>

#include <cstdlib> // std::system

#ifdef _WIN32
#include <Windows.h>
#endif

std::pair<QString /* exe path */, QString /* args */> OsShell::shellExecutable()
{
#ifdef _WIN32
	const QString defaultShell = QLatin1String("powershell.exe");
	QString shell = CSettings{}.value(KEY_OTHER_SHELL_COMMAND_NAME, defaultShell).toString();
	QStringList argsList = QProcess::splitCommand(shell);
	assert_and_return_r(!argsList.empty(), {});
	shell = std::move(argsList.front());
	argsList.pop_front();

	QString argsString;
	for (auto&& str : argsList)
	{
		if (!argsString.isEmpty())
			argsString += ' ';

		if (str.contains(' '))
			argsString += ('\"' % str % '\"');
		else
			argsString += str;
	}

	return { shell, argsString };

#elif defined __APPLE__
	const QString shellCommand = CSettings().value(KEY_OTHER_SHELL_COMMAND_NAME, "/Applications/Utilities/Terminal.app/Contents/MacOS/Terminal").toString();
	return { shellCommand , {} };
#elif defined __linux__ || defined __FreeBSD__
	const QString consoleExecutable = CSettings().value(KEY_OTHER_SHELL_COMMAND_NAME).toString();
	if (QFileInfo(consoleExecutable).exists())
		return { consoleExecutable, {} };

	static constexpr const char* knownTerminals[] {
		"/usr/bin/konsole", // KDE
		"/usr/bin/gnome-terminal", // Gnome
		"/usr/bin/pantheon-terminal", // Pantheon (Elementary OS)
		"/usr/bin/qterminal", // QTerminal under linux
		"/usr/local/bin/qterminal" // QTerminal under freebsd
	};

	for (const auto& candidate: knownTerminals)
		if (QFileInfo(candidate).exists())
			return { candidate, {} };

	return {};
#else
	#error unknown platform
#endif
}

void OsShell::executeShellCommand(const QString& command, const QString& workingDir)
{
	std::thread([command, workingDir](){
	#ifdef _WIN32
		WCHAR commandString[32768] = { 0 };
		const auto len = (QStringLiteral("pushd ") + workingDir + " && " + command).toWCharArray(commandString);
		//const auto len = QString{"cmd /c \"%1\""}.arg(command).toWCharArray(commandString);
		assert_and_return_r(static_cast<size_t>(len) < std::size(commandString), );
		::_wsystem(commandString);
	#else
		const QString commandLine = "cd " % escapedPath(workingDir) % " && " % command;
		const int result = std::system(commandLine.toUtf8().constData());
		if (result != 0)
			qInfo().noquote() << "The command failed with code " << result << '\n' << commandLine;
	#endif
	}).detach();
}

#ifdef _WIN32
#include "windows/windowsutils.h"

#include <shellapi.h>

bool OsShell::runExecutable(const QString& command, const QString& arguments, const QString& workingDir)
{
	return runExe(command, arguments, workingDir, false);
}

bool OsShell::runExe(const QString& command, const QString& arguments, const QString& workingDir, bool asAdmin)
{
	SHELLEXECUTEINFOW shExecInfo = {0};
	shExecInfo.cbSize = sizeof(SHELLEXECUTEINFOW);

	const QString commandPathUnc = toUncPath(command);
	const QString workingDirNative = toNativeSeparators(workingDir);

	shExecInfo.fMask = SEE_MASK_FLAG_NO_UI;
	shExecInfo.hwnd = nullptr;
	shExecInfo.lpVerb = asAdmin ? L"runas" : L"open";
	shExecInfo.lpFile = (WCHAR*)commandPathUnc.utf16();
	shExecInfo.lpParameters = arguments.isEmpty() ? nullptr : (WCHAR*)arguments.utf16();
	shExecInfo.lpDirectory = (WCHAR*)workingDirNative.utf16();
	shExecInfo.nShow = SW_SHOWNORMAL;
	shExecInfo.hInstApp = nullptr;

	if (ShellExecuteExW(&shExecInfo) == 0)
	{
		if (GetLastError() != ERROR_CANCELLED) // Operation canceled by the user
		{
			const QString errorString = QString::fromStdString(ErrorStringFromLastError());
			qInfo() << "ShellExecuteExW failed when trying to run" << commandPathUnc << "in" << workingDirNative;
			qInfo() << errorString;

			return false;
		}
	}

	return true;
}
#else
bool OsShell::runExecutable(const QString & command, const QString & parameters, const QString & workingDir)
{
	return QProcess::startDetached(command, QStringList() << parameters, workingDir);
}
#endif

#ifdef _WIN32
#include <Shobjidl.h>
#include <ShlObj.h>
#include <windowsx.h>

class CItemIdListReleaser {
public:
	explicit CItemIdListReleaser(ITEMIDLIST * idList) : _idList(idList) {}
	~CItemIdListReleaser() { if (_idList) CoTaskMemFree(_idList); }
private:
	ITEMIDLIST * _idList;
};

class CComInterfaceReleaser {
public:
	explicit CComInterfaceReleaser(IUnknown * i) : _i(i) {}
	~CComInterfaceReleaser() { if (_i) _i->Release(); }
private:
	IUnknown * _i;
};

class CItemIdArrayReleaser {
public:
	explicit CItemIdArrayReleaser(const std::vector<ITEMIDLIST*>& idArray) : _array(idArray) {}
	~CItemIdArrayReleaser() {
		for (ITEMIDLIST* item: _array)
			CoTaskMemFree(item);
	}

	CItemIdArrayReleaser& operator=(const CItemIdArrayReleaser&) = delete;
private:
	const std::vector<ITEMIDLIST*>& _array;
};

bool prepareContextMenuForObjects(std::vector<std::wstring> objects, void* parentWindow, HMENU& hmenu, IContextMenu*& imenu);

// Pos must be global

bool OsShell::openShellContextMenuForObjects(const std::vector<std::wstring>& objects, int xPos, int yPos, void * parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	IContextMenu * imenu = 0;
	HMENU hMenu = NULL;
	if (!prepareContextMenuForObjects(objects, parentWindow, hMenu, imenu) || !hMenu || !imenu)
		return false;

	CComInterfaceReleaser menuReleaser(imenu);

	const int iCmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD, xPos, yPos, (HWND)parentWindow, NULL);
	if (iCmd > 0)
	{
		CMINVOKECOMMANDINFO info = { 0 };
		info.cbSize = sizeof(info);
		info.hwnd = (HWND)parentWindow;
		info.lpVerb  = MAKEINTRESOURCEA(iCmd - 1);
		info.nShow = SW_SHOWNORMAL;
		imenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
	}

	DestroyMenu(hMenu);

	return true;
}

bool OsShell::copyObjectsToClipboard(const std::vector<std::wstring>& objects, void * parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	IContextMenu * imenu = 0;
	HMENU hMenu = NULL;
	if (!prepareContextMenuForObjects(objects, parentWindow, hMenu, imenu) || !hMenu || !imenu)
		return false;

	CComInterfaceReleaser menuReleaser(imenu);

	const char command[] = "Copy";

	CMINVOKECOMMANDINFO info = { 0 };
	info.cbSize = sizeof(info);
	info.hwnd = (HWND)parentWindow;
	info.lpVerb = command;
	info.nShow = SW_SHOWNORMAL;
	const auto result = imenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);

	DestroyMenu(hMenu);

	return SUCCEEDED(result);
}

bool OsShell::cutObjectsToClipboard(const std::vector<std::wstring>& objects, void * parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	IContextMenu * imenu = 0;
	HMENU hMenu = NULL;
	if (!prepareContextMenuForObjects(objects, parentWindow, hMenu, imenu) || !hMenu || !imenu)
		return false;

	CComInterfaceReleaser menuReleaser(imenu);

	const char command [] = "Cut";

	CMINVOKECOMMANDINFO info = { 0 };
	info.cbSize = sizeof(info);
	info.hwnd = (HWND) parentWindow;
	info.lpVerb = command;
	info.nShow = SW_SHOWNORMAL;
	const auto result = imenu->InvokeCommand((LPCMINVOKECOMMANDINFO) &info);

	DestroyMenu(hMenu);

	return SUCCEEDED(result);
}

bool OsShell::pasteFilesAndFoldersFromClipboard(std::wstring destFolder, void * parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	IContextMenu * imenu = 0;
	HMENU hMenu = NULL;
	if (!prepareContextMenuForObjects(std::vector<std::wstring>(1, destFolder), parentWindow, hMenu, imenu) || !hMenu || !imenu)
		return false;

	CComInterfaceReleaser menuReleaser(imenu);

	const char command[] = "Paste";

	CMINVOKECOMMANDINFO info = { 0 };
	info.cbSize = sizeof(info);
	info.hwnd = (HWND) parentWindow;
	info.lpVerb = command;
	info.nShow = SW_SHOWNORMAL;
	const auto result = imenu->InvokeCommand((LPCMINVOKECOMMANDINFO) &info);

	DestroyMenu(hMenu);

	return SUCCEEDED(result);
}

std::wstring OsShell::toolTip(std::wstring itemPath)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	std::replace(itemPath.begin(), itemPath.end(), '/', '\\');
	std::wstring tipString;
	ITEMIDLIST * id = 0;
	HRESULT result = SHParseDisplayName(itemPath.c_str(), 0, &id, 0, 0);
	if (!SUCCEEDED(result) || !id)
		return tipString;
	CItemIdListReleaser idReleaser (id);

	LPCITEMIDLIST child = 0;
	IShellFolder * ifolder = 0;
	result = SHBindToParent(id, IID_IShellFolder, (void**)&ifolder, &child);
	if (!SUCCEEDED(result) || !child)
		return tipString;

	IQueryInfo* iQueryInfo = 0;
	if (SUCCEEDED(ifolder->GetUIObjectOf(NULL, 1, &child, IID_IQueryInfo, 0, (void**)&iQueryInfo)) && iQueryInfo)
	{
		LPWSTR lpszTip = 0;
		CComInterfaceReleaser releaser (iQueryInfo);
		if (SUCCEEDED(iQueryInfo->GetInfoTip(0, &lpszTip)) && lpszTip)
		{
			tipString = lpszTip;
			CoTaskMemFree(lpszTip);
		}
	}

	std::replace(tipString.begin(), tipString.end(), '\r', '\n');
	return tipString;
}

bool OsShell::deleteItems(const std::vector<std::wstring>& items, bool moveToTrash, void * parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	assert_r(parentWindow);
	std::vector<LPITEMIDLIST> idLists;
	for (auto& path: items)
	{
		LPITEMIDLIST idl = ILCreateFromPathW(path.c_str());
		if (!idl)
		{
			for (auto& pid : idLists)
				ILFree(pid);

			qInfo() << "ILCreateFromPathW" << "failed for path" << QString::fromWCharArray(path.c_str());
			return false;
		}
		idLists.push_back(idl);
		assert_r(idLists.back());
	}

	IShellItemArray * iArray = 0;
	HRESULT result = SHCreateShellItemArrayFromIDLists((UINT)idLists.size(), (LPCITEMIDLIST*)idLists.data(), &iArray);

	// Freeing memory
	for (auto& pid: idLists)
		ILFree(pid);
	idLists.clear();

	if (!SUCCEEDED(result) || !iArray)
	{
		qInfo() << "SHCreateShellItemArrayFromIDLists failed";
		return false;
	}

	IFileOperation * iOperation = 0;
	result = CoCreateInstance(CLSID_FileOperation, 0, CLSCTX_ALL, IID_IFileOperation, (void**)&iOperation);
	if (!SUCCEEDED(result) || !iOperation)
	{
		qInfo() << "CoCreateInstance(CLSID_FileOperation, 0, CLSCTX_ALL, IID_IFileOperation, (void**)&iOperation) failed";
		return false;
	}

	result = iOperation->DeleteItems(iArray);
	if (!SUCCEEDED(result))
	{
		qInfo() << "DeleteItems failed";
	}
	else
	{
		if (moveToTrash)
		{
			result = iOperation->SetOperationFlags(FOF_ALLOWUNDO);
		}
		else
			result = iOperation->SetOperationFlags(FOF_WANTNUKEWARNING);

		if (!SUCCEEDED(result))
			qInfo() << "SetOperationFlags failed";

		result = iOperation->SetOwnerWindow((HWND) parentWindow);
		if (!SUCCEEDED(result))
			qInfo() << "SetOwnerWindow failed";

		result = iOperation->PerformOperations();
		if (!SUCCEEDED(result) && result != COPYENGINE_E_USER_CANCELLED)
		{
			qInfo() << "PerformOperations failed";
			if (result == COPYENGINE_E_REQUIRES_ELEVATION)
				qInfo() << "Elevation required";
		}
		else
			result = S_OK;
	}

	iOperation->Release();
	iArray->Release();
	return SUCCEEDED(result);
}

bool OsShell::recycleBinContextMenu(int xPos, int yPos, void *parentWindow)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	PIDLIST_ABSOLUTE idlist = 0;
	if (!SUCCEEDED(SHGetFolderLocation(0, CSIDL_BITBUCKET, 0, 0, &idlist)))
		return false;

	IShellFolder * iFolder = 0;
	LPCITEMIDLIST list;
	HRESULT result = SHBindToParent(idlist, IID_IShellFolder, (void**)&iFolder, &list);
	if (!SUCCEEDED(result) || !list || !iFolder)
		return false;

	IContextMenu * imenu = 0;
	result = iFolder->GetUIObjectOf((HWND)parentWindow, 1u, &list, IID_IContextMenu, 0, (void**)&imenu);
	CoTaskMemFree(idlist);
	if (!SUCCEEDED(result) || !imenu)
		return false;
	CComInterfaceReleaser menuReleaser(imenu);

	HMENU hMenu = CreatePopupMenu();
	if (!hMenu)
		return false;
	if (SUCCEEDED(imenu->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL)))
	{
		int iCmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD, xPos, yPos, (HWND)parentWindow, NULL);
		if (iCmd > 0)
		{
			CMINVOKECOMMANDINFOEX info = { 0 };
			info.cbSize = sizeof(info);
			info.fMask = CMIC_MASK_UNICODE;
			info.hwnd = (HWND)parentWindow;
			info.lpVerb  = MAKEINTRESOURCEA(iCmd - 1);
			info.lpVerbW = MAKEINTRESOURCEW(iCmd - 1);
			info.nShow = SW_SHOWNORMAL;
			imenu->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
		}
	}
	DestroyMenu(hMenu);
	return true;
}

bool prepareContextMenuForObjects(std::vector<std::wstring> objects, void * parentWindow, HMENU& hmenu, IContextMenu*& imenu)
{
	CO_INIT_HELPER(COINIT_APARTMENTTHREADED);

	if (objects.empty())
		return false;

	std::vector<ITEMIDLIST*> ids;
	std::vector<LPCITEMIDLIST> relativeIds;
	IShellFolder * ifolder = 0;
	for (size_t i = 0, nItems = objects.size(); i < nItems; ++i)
	{
		auto& item = objects[i];
		std::replace(item.begin(), item.end(), '/', '\\');
		//item.pop_back(); // TODO: ???
		ids.emplace_back(nullptr);
		HRESULT result = SHParseDisplayName(item.c_str(), nullptr, &ids.back(), 0, nullptr); // TODO: avoid c_str() somehow?
		if (!SUCCEEDED(result) || !ids.back())
		{
			ids.pop_back();
			continue;
		}

		relativeIds.emplace_back(nullptr);
		result = SHBindToParent(ids.back(), IID_IShellFolder, (void**)&ifolder, &relativeIds.back());
		if (!SUCCEEDED(result) || !relativeIds.back())
			relativeIds.pop_back();
		else if (i < nItems - 1 && ifolder)
		{
			ifolder->Release();
			ifolder = nullptr;
		}
	}

	CItemIdArrayReleaser arrayReleaser(ids);

	assert_r(parentWindow);
	assert_and_return_message_r(ifolder, "Error getting ifolder", false);
	assert_and_return_message_r(!relativeIds.empty(), "RelativeIds is empty", false);

	imenu = 0;
	HRESULT result = ifolder->GetUIObjectOf((HWND)parentWindow, (UINT)relativeIds.size(), (const ITEMIDLIST **)relativeIds.data(), IID_IContextMenu, 0, (void**)&imenu);
	if (!SUCCEEDED(result) || !imenu)
		return false;

	hmenu = CreatePopupMenu();
	if (!hmenu)
		return false;
	return (SUCCEEDED(imenu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL)));
}

#elif defined __linux__ || __FreeBSD__

bool OsShell::openShellContextMenuForObjects(const std::vector<std::wstring>& /*objects*/, int /*xPos*/, int /*yPos*/, void * /*parentWindow*/)
{
	return false;
}

std::wstring OsShell::toolTip(std::wstring /*itemPath*/)
{
	return std::wstring();
}

bool OsShell::recycleBinContextMenu(int /*xPos*/, int /*yPos*/, void * /*parentWindow*/)
{
	return true;
}

#elif defined __APPLE__

bool OsShell::openShellContextMenuForObjects(const std::vector<std::wstring>& /*objects*/, int /*xPos*/, int /*yPos*/, void * /*parentWindow*/)
{
	return false;
}

std::wstring OsShell::toolTip(std::wstring /*itemPath*/)
{
	return std::wstring();
}

bool OsShell::recycleBinContextMenu(int /*xPos*/, int /*yPos*/, void */*parentWindow*/)
{
	return true;
}

#else
#error unsupported platform
#endif
