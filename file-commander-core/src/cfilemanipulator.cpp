#include "cfilemanipulator.h"
#include "filesystemhelperfunctions.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QFile>
#include <QFileInfo>
RESTORE_COMPILER_WARNINGS

#ifdef _WIN32
#include "windows/windowsutils.h"

#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// Operations
CFileManipulator::CFileManipulator(const CFileSystemObject& object) : _object(object)
{
}

FileOperationResultCode CFileManipulator::copyAtomically(const QString& destFolder, const QString& newName, bool transferPermissions)
{
	assert_r(_object.isFile());
	assert_r(QFileInfo{destFolder}.isDir());

	QFile file(_object.fullAbsolutePath());
	const QString newFilePath = destFolder + (newName.isEmpty() ? _object.fullName() : newName);
	bool succ = file.copy(newFilePath);
	if (succ)
	{
		if (transferPermissions)
		{
			_lastErrorMessage = copyPermissions(file, newFilePath);
			succ = _lastErrorMessage.isEmpty();
		}
	}
	else
		_lastErrorMessage = file.errorString();

	return succ ? FileOperationResultCode::Ok : FileOperationResultCode::Fail;
}

FileOperationResultCode CFileManipulator::moveAtomically(const QString& location, const QString& newName)
{
	if (!_object.exists())
		return FileOperationResultCode::ObjectDoesntExist;
	else if (_object.isCdUp())
		return FileOperationResultCode::Fail;

	assert_r(QFileInfo(location).isDir());
	const QString fullNewName = location % '/' % (newName.isEmpty() ? _object.fullName() : newName);
	const CFileSystemObject destInfo(fullNewName);
	const bool newNameDiffersOnlyInLetterCase = destInfo.fullAbsolutePath().compare(_object.fullAbsolutePath(), Qt::CaseInsensitive) == 0;
	if ((caseSensitiveFilesystem() || !newNameDiffersOnlyInLetterCase) && destInfo.exists())
	{
		// If the file system is case-insensitive, and the source and destination only differ by case, renaming is allowed even though formally the destination already exists (fix for #102)
		if (_object.isDir() || destInfo.isFile())
			return FileOperationResultCode::TargetAlreadyExists;
	}

	// Special case for Windows, where QFile::rename and QDir::rename fail to handle names that only differ by letter case (https://bugreports.qt.io/browse/QTBUG-3570)
#ifdef _WIN32
	if (newNameDiffersOnlyInLetterCase)
	{
		if (MoveFileW((const WCHAR*)_object.fullAbsolutePath().utf16(), (const WCHAR*)destInfo.fullAbsolutePath().utf16()) != 0)
			return FileOperationResultCode::Ok;

		_lastErrorMessage = ErrorStringFromLastError();
		return FileOperationResultCode::Fail;
	}
#endif

	if (_object.isFile())
	{
		QFile file(_object.fullAbsolutePath());
		const bool succ = file.rename(fullNewName);
		if (!succ)
		{
			_lastErrorMessage = file.errorString();
			return FileOperationResultCode::Fail;
		}

		_object.refreshInfo();
		return FileOperationResultCode::Ok;
	}
	else if (_object.isDir())
	{
		return QDir().rename(_object.fullAbsolutePath(), fullNewName) ? FileOperationResultCode::Ok : FileOperationResultCode::Fail;
	}
	else
		return FileOperationResultCode::Fail;
}

FileOperationResultCode CFileManipulator::copyAtomically(const CFileSystemObject& object, const QString& destFolder, const QString& newName /*= QString()*/)
{
	return CFileManipulator(object).copyAtomically(destFolder, newName);
}

FileOperationResultCode CFileManipulator::moveAtomically(const CFileSystemObject& object, const QString& destFolder, const QString& newName /*= QString()*/)
{
	return CFileManipulator(object).moveAtomically(destFolder, newName);
}

// Non-blocking file copy API

// Requests copying the next (or the first if copyOperationInProgress() returns false) chunk of the file.
FileOperationResultCode CFileManipulator::copyChunk(size_t chunkSize, const QString& destFolder, const QString& newName /*= QString()*/, bool transferPermissions)
{
	assert_r(bool(_thisFile) == bool(_destFile));
	assert_r(_object.isFile());
	assert_r(QFileInfo(destFolder).isDir());

	if (!copyOperationInProgress())
	{
		_pos = 0;

		// Creating files
		_thisFile = std::make_shared<QFile>(_object.fullAbsolutePath());
		_destFile = std::make_shared<QFile>(destFolder + (newName.isEmpty() ? _object.fullName() : newName));

		// Initializing - opening files
		if (!_thisFile->open(QFile::ReadOnly))
		{
			_lastErrorMessage = _thisFile->errorString();

			_thisFile.reset();
			_destFile.reset();

			return FileOperationResultCode::Fail;
		}

		if (!_destFile->open(QFile::ReadWrite))
		{
			_lastErrorMessage = _destFile->errorString();

			_thisFile.reset();
			_destFile.reset();

			return FileOperationResultCode::Fail;
		}

		if (!_destFile->resize(_object.size()))
		{
			_lastErrorMessage.clear(); // QFile provides no meaningful message for this case.
			_destFile->close();
			assert_r(_destFile->remove());

			_thisFile.reset();
			_destFile.reset();

			return FileOperationResultCode::NotEnoughSpaceAvailable;
		}
	}

	assert_r(_destFile->isOpen() == _thisFile->isOpen());

	const auto actualChunkSize = std::min(chunkSize, (size_t)(_object.size() - _pos));

	if (actualChunkSize != 0)
	{
		const auto src = _thisFile->map(_pos, actualChunkSize);
		if (!src)
		{
			_lastErrorMessage = _thisFile->errorString();
			return FileOperationResultCode::Fail;
		}

		const auto dest = _destFile->map(_pos, actualChunkSize);
		if (!dest)
		{
			_lastErrorMessage = _destFile->errorString();
			return FileOperationResultCode::Fail;
		}

		::memcpy(dest, src, actualChunkSize);
		_pos += actualChunkSize;

		_thisFile->unmap(src);
		_destFile->unmap(dest);
	}

	if (actualChunkSize < chunkSize)
	{
		// Copying complete
		if (transferPermissions)
			_lastErrorMessage = copyPermissions(*_thisFile, *_destFile);

		_thisFile.reset();
		_destFile.reset();

		if (transferPermissions && !_lastErrorMessage.isEmpty())
			return FileOperationResultCode::Fail;
	}

	return FileOperationResultCode::Ok;
}

FileOperationResultCode CFileManipulator::moveChunk(uint64_t /*chunkSize*/, const QString &destFolder, const QString& newName)
{
	return moveAtomically(destFolder, newName);
}

bool CFileManipulator::copyOperationInProgress() const
{
	if (!_destFile && !_thisFile)
		return false;

	assert_r(_destFile->isOpen() == _thisFile->isOpen());
	return _destFile->isOpen() && _thisFile->isOpen();
}

uint64_t CFileManipulator::bytesCopied() const
{
	return _pos;
}

FileOperationResultCode CFileManipulator::cancelCopy()
{
	if (!copyOperationInProgress())
		return FileOperationResultCode::Ok;

	_thisFile->close();
	_destFile->close();

	const bool succ = _destFile->remove();
	_thisFile.reset();
	_destFile.reset();
	return succ ? FileOperationResultCode::Ok : FileOperationResultCode::Fail;
}

bool CFileManipulator::makeWritable(bool writable)
{
	assert_and_return_message_r(_object.isFile(), "This method only works for files", false);

#ifdef _WIN32
	const QString UNCPath = toUncPath(_object.fullAbsolutePath());
	const DWORD attributes = GetFileAttributesW((LPCWSTR)UNCPath.utf16());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		_lastErrorMessage = ErrorStringFromLastError();
		return false;
	}

	if (SetFileAttributesW((LPCWSTR) UNCPath.utf16(), writable ? (attributes & (~(uint32_t)FILE_ATTRIBUTE_READONLY)) : (attributes | FILE_ATTRIBUTE_READONLY)) != TRUE)
	{
		_lastErrorMessage = ErrorStringFromLastError();
		return false;
	}

	return true;
#else
	struct stat fileInfo;

	const QByteArray fileName = _object.fullAbsolutePath().toUtf8();
	if (stat(fileName.constData(), &fileInfo) != 0)
	{
		_lastErrorMessage = strerror(errno);
		return false;
	}

	if (chmod(fileName.constData(), writable ? (fileInfo.st_mode | S_IWUSR) : (fileInfo.st_mode & (~S_IWUSR))) != 0)
	{
		_lastErrorMessage = strerror(errno);
		return false;
	}

	return true;
#endif
}

bool CFileManipulator::makeWritable(const CFileSystemObject& object, bool writable /*= true*/)
{
	return CFileManipulator(object).makeWritable(writable);
}

FileOperationResultCode CFileManipulator::remove()
{
	assert_and_return_message_r(_object.exists(), "Object doesn't exist", FileOperationResultCode::ObjectDoesntExist);

	if (_object.isFile())
	{
		QFile file(_object.fullAbsolutePath());
		if (file.remove())
			return FileOperationResultCode::Ok;
		else
		{
			_lastErrorMessage = file.errorString();
			return FileOperationResultCode::Fail;
		}
	}
	else if (_object.isDir())
	{
		assert_r(_object.isEmptyDir());
		errno = 0;
		if (!QDir{_object.fullAbsolutePath()}.rmdir("."))
		{
#if defined __linux || defined __APPLE__ || defined __FreeBSD__
			if (::rmdir(_object.fullAbsolutePath().toLocal8Bit().constData()) == 0)
				return FileOperationResultCode::Ok;

			_lastErrorMessage = strerror(errno);
			return FileOperationResultCode::Fail;
#else
			return FileOperationResultCode::Fail;
#endif
		}
		return FileOperationResultCode::Ok;
	}
	else
		return FileOperationResultCode::Fail;
}

FileOperationResultCode CFileManipulator::remove(const CFileSystemObject& object)
{
	return CFileManipulator(object).remove();
}

QString CFileManipulator::lastErrorMessage() const
{
	return _lastErrorMessage;
}

QString CFileManipulator::copyPermissions(const QFile &sourceFile, QFile &destinationFile)
{
	if (!destinationFile.setPermissions(sourceFile.permissions())) // TODO: benchmark against the static version QFile::setPermissions(filePath, permissions)
		return destinationFile.errorString();
	else
		return {};
}

QString CFileManipulator::copyPermissions(const QFile &sourceFile, const QString &destinationFilePath)
{
	QFile destinationFile {destinationFilePath};
	return copyPermissions(sourceFile, destinationFile);
}
