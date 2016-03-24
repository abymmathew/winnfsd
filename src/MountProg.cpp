#include "MountProg.h"
#include "FileTable.h"
#include <string.h>
#include <map>
#include <fstream>
#include <iostream>
#include <string>
#include <direct.h>

enum
{
    MOUNTPROC_NULL = 0,
    MOUNTPROC_MNT = 1,
    MOUNTPROC_DUMP = 2,
    MOUNTPROC_UMNT = 3,
    MOUNTPROC_UMNTALL = 4,
    MOUNTPROC_EXPORT = 5
};

enum
{
    MNT_OK = 0,
    MNTERR_PERM = 1,
    MNTERR_NOENT = 2,
    MNTERR_IO = 5,
    MNTERR_ACCESS = 13,
    MNTERR_NOTDIR = 20,
    MNTERR_INVAL = 22
};

typedef void (CMountProg::*PPROC)(void);

CMountProg::CMountProg() : CRPCProg()
{
    m_nMountNum = 0;
    m_pPathFile = NULL;
    memset(m_pClientAddr, 0, sizeof(m_pClientAddr));
}

CMountProg::~CMountProg()
{
    int i;

    if (m_pPathFile) {
        free(m_pPathFile);
        m_pPathFile = NULL;
    }	

    for (i = 0; i < MOUNT_NUM_MAX; i++) {
        delete[] m_pClientAddr[i];
    }

}

bool CMountProg::SetPathFile(char *file)
{
	char *formattedFile = FormatPath(file, FORMAT_PATH);

	if (!formattedFile) {
		return false;
	}

	std::ifstream pathFile(formattedFile);

	if (pathFile.good()) {
		pathFile.close();
		if (m_pPathFile) {
			free(m_pPathFile);
		}
		m_pPathFile = formattedFile;
		return true;
	}

	pathFile.close();
	free(formattedFile);
	return false;
}

void CMountProg::Export(char *path, char *pathAlias)
{
	char *formattedPath = FormatPath(path, FORMAT_PATH);

	if (formattedPath) {
		pathAlias = FormatPathAlias(pathAlias);

		if (m_PathMap.count(pathAlias) == 0) {
			m_PathMap[pathAlias] = formattedPath;
			printf("Path #%i is: %s, path alias is: %s\n", m_PathMap.size(), path, pathAlias);
		} else {
			printf("Path %s with path alias  %s already known\n", path, pathAlias);
		}

		free(formattedPath);
	}

}

bool CMountProg::Refresh()
{
	if (m_pPathFile != NULL) {
		ReadPathsFromFile(m_pPathFile);
		return true;
	}

	return false;
}

int CMountProg::GetMountNumber(void)
{
    return m_nMountNum;  //the number of clients mounted
}

char *CMountProg::GetClientAddr(int nIndex)
{
    int i;

    if (nIndex < 0 || nIndex >= m_nMountNum) {
        return NULL;
    }

    for (i = 0; i < MOUNT_NUM_MAX; i++) {
        if (m_pClientAddr[i] != NULL) {
            if (nIndex == 0) {
                return m_pClientAddr[i];  //client address
            } else {
                --nIndex;
            }
        }

    }
    return NULL;
}

int CMountProg::Process(IInputStream *pInStream, IOutputStream *pOutStream, ProcessParam *pParam)
{
    static PPROC pf[] = { &CMountProg::ProcedureNULL, &CMountProg::ProcedureMNT, &CMountProg::ProcedureNOIMP, &CMountProg::ProcedureUMNT };

    PrintLog("MOUNT ");

    if (pParam->nProc >= sizeof(pf) / sizeof(PPROC)) {
        ProcedureNOIMP();
        PrintLog("\n");
        return PRC_NOTIMP;
    }

    m_pInStream = pInStream;
    m_pOutStream = pOutStream;
    m_pParam = pParam;
    m_nResult = PRC_OK;
    (this->*pf[pParam->nProc])();
    PrintLog("\n");

    return m_nResult;
}

void CMountProg::ProcedureNULL(void)
{
    PrintLog("NULL");
}

void CMountProg::ProcedureMNT(void)
{
	Refresh();
    char *path = new char[MAXPATHLEN + 1];
	int i;

	PrintLog("MNT");
	PrintLog(" from %s\n", m_pParam->pRemoteAddr);

	if (GetPath(&path)) {
		m_pOutStream->Write(MNT_OK); //OK

		if (m_pParam->nVersion == 1) {
			m_pOutStream->Write(GetFileHandle(path), FHSIZE);  //fhandle
		} else {
			m_pOutStream->Write(NFS3_FHSIZE);  //length
			m_pOutStream->Write(GetFileHandle(path), NFS3_FHSIZE);  //fhandle
			m_pOutStream->Write(0);  //flavor
		}

		++m_nMountNum;

		for (i = 0; i < MOUNT_NUM_MAX; i++) {
			if (m_pClientAddr[i] == NULL) { //search an empty space
				m_pClientAddr[i] = new char[strlen(m_pParam->pRemoteAddr) + 1];
				strcpy_s(m_pClientAddr[i], (strlen(m_pParam->pRemoteAddr) + 1), m_pParam->pRemoteAddr);  //remember the client address
				break;
			}
		}
	} else {
		m_pOutStream->Write(MNTERR_ACCESS);  //permission denied
    }
}

void CMountProg::ProcedureUMNT(void)
{
	char *path = new char[MAXPATHLEN + 1];
    int i;

    PrintLog("UMNT");
    GetPath(&path);
    PrintLog(" from %s", m_pParam->pRemoteAddr);

    for (i = 0; i < MOUNT_NUM_MAX; i++) {
        if (m_pClientAddr[i] != NULL) {
            if (strcmp(m_pParam->pRemoteAddr, m_pClientAddr[i]) == 0) { //address match
                delete[] m_pClientAddr[i];  //remove this address
                m_pClientAddr[i] = NULL;
                --m_nMountNum;
                break;
            }
        }
    }
}

void CMountProg::ProcedureNOIMP(void)
{
    PrintLog("NOIMP");
    m_nResult = PRC_NOTIMP;
}

bool CMountProg::GetPath(char **returnPath)
{
	unsigned long i, nSize;
	static char path[MAXPATHLEN + 1];
	static char finalPath[MAXPATHLEN + 1];
	bool foundPath = false;

	m_pInStream->Read(&nSize);

	if (nSize > MAXPATHLEN) {
		nSize = MAXPATHLEN;
	}

	typedef std::map<std::string, std::string>::iterator it_type;
	m_pInStream->Read(path, nSize);

	for (it_type iterator = m_PathMap.begin(); iterator != m_PathMap.end(); iterator++) {
		char* pathAlias = const_cast<char*>(iterator->first.c_str());
		char* windowsPath = const_cast<char*>(iterator->second.c_str());

		size_t aliasPathSize = strlen(pathAlias);
		size_t windowsPathSize = strlen(windowsPath);
		size_t requestedPathSize = nSize;

		if ((requestedPathSize < windowsPathSize) && (strncmp(path, pathAlias, aliasPathSize) == 0)) {
			foundPath = true;
			//The requested path starts with the alias. Let's replace the alias with the real path
			strncpy_s(finalPath, windowsPath, sizeof(finalPath));
			//strncpy_s(finalPath + windowsPathSize, (path + aliasPathSize), (sizeof(finalPath)-windowsPathSize));
			finalPath[windowsPathSize + requestedPathSize - aliasPathSize] = '\0';

			for (i = 0; i < requestedPathSize; i++) { //transform path to Windows format
				if (finalPath[windowsPathSize + i] == '/') {
					finalPath[windowsPathSize + i] = '\\';
				}
			}
		} else if ((strlen(path) == strlen(pathAlias)) && (strncmp(path, pathAlias, aliasPathSize) == 0)) {
			foundPath = true;
			//The requested path IS the alias
			strncpy_s(finalPath, windowsPath, sizeof(finalPath));
			finalPath[windowsPathSize] = '\0';
		} else if ((strlen(path) == strlen(windowsPath)) && (strncmp(path, pathAlias, windowsPathSize) == 0)) {
			foundPath = true;
			//The requested path does not start with the alias, let's treat it normally
			strncpy_s(finalPath, path, sizeof(finalPath));
			finalPath[0] = finalPath[1];  //transform mount path to Windows format
			finalPath[1] = ':';

			for (i = 2; i < nSize; i++) {
				if (finalPath[i] == '/') {
					finalPath[i] = '\\';
				}
			}

			finalPath[nSize] = '\0';
		}

		if (foundPath == true) {
			break;
		}
	}

	PrintLog("Final local requested path: %s\n", finalPath);

	if ((nSize & 3) != 0) {
		m_pInStream->Read(&i, 4 - (nSize & 3));  //skip opaque bytes
	}

	*returnPath = finalPath;
	return foundPath;
}


bool CMountProg::ReadPathsFromFile(char* sFileName)
{
	std::ifstream pathFile(sFileName);

	if (pathFile.is_open()) {
		std::string line;

		while (std::getline(pathFile, line)) {
			char *pCurPath = (char*)malloc(line.size() + 1);
			pCurPath = (char*)line.c_str();

			if (pCurPath != NULL) {
				char curPathAlias[MAXPATHLEN];
				strcpy_s(curPathAlias, pCurPath);
				char *pCurPathAlias = (char*)malloc(strlen(curPathAlias));
				pCurPathAlias = curPathAlias;

				Export(pCurPath, pCurPathAlias);
			}
		}
	} else {
		printf("Can't open file %s.\n", sFileName);
		return false;
	}

	return true;
}

char *CMountProg::FormatPath(char *pPath, pathFormats format)
{
    size_t len = strlen(pPath);

	//Remove head spaces
	while (*pPath == ' ') {
		++pPath;
		len--;
	}

	//Remove tail spaces
	while (len > 0 && *(pPath + len - 1) == ' ') {
		len--;
	}

	//Is comment?
	if (*pPath == '#') {
		return NULL;
	}

	//Remove head "
	if (*pPath == '"') {
		++pPath;
		len--;
	}

	//Remove tail "
	if (len > 0 && *(pPath + len - 1) == '"') {
		len--;
	}

	if (len < 1) {
		return NULL;
	}

	char *result = (char *)malloc(len + 1);
	strncpy_s(result, len + 1, pPath, len);

	//Check for right path format
	if (format == FORMAT_PATH) {
		if (result[0] == '.') {
			static char path1[MAXPATHLEN];
			_getcwd(path1, MAXPATHLEN);

			if (result[1] == '\0') {
				len = strlen(path1);
				result = (char *)realloc(result, len + 1);
				strcpy_s(result, len + 1, path1);
			} else if (result[1] == '\\') {
				strcat_s(path1, result + 1);
				len = strlen(path1);
				result = (char *)realloc(result, len + 1);
				strcpy_s(result, len + 1, path1);
			}

		}
		if (len >= 2 && result[1] == ':' && ((result[0] >= 'A' && result[0] <= 'Z') || (result[0] >= 'a' && result[0] <= 'z'))) { //check path format
			char tempPath[MAXPATHLEN] = "\\\\?\\";
			strcat_s(tempPath, result);
			len = strlen(tempPath);
			result = (char *)realloc(result, len + 1);
			strcpy_s(result, len + 1, tempPath);
		}

		if (len < 6 || result[5] != ':' || !((result[4] >= 'A' && result[4] <= 'Z') || (result[4] >= 'a' && result[4] <= 'z'))) { //check path format
			printf("Path %s format is incorrect.\n", pPath);
			printf("Please use a full path such as C:\\work or \\\\?\\C:\\work\n");
			free(result);
			return NULL;
		}

		for (size_t i = 0; i < len; i++) {
			if (result[i] == '/') {
				result[i] = '\\';
			}
		}
	} else if (format == FORMAT_PATHALIAS) {
		if (result[0] != '/') { //check path alias format
			printf("Path alias format is incorrect.\n");
			printf("Please use a path like /exports\n");
			free(result);
			return NULL;
		}
	}

	return result;
}

char *CMountProg::FormatPathAlias(char *pPathAlias)
{
	pPathAlias[1] = pPathAlias[0]; //transform mount path to Windows format
	pPathAlias[0] = '/';

	for (size_t i = 2; i < strlen(pPathAlias); i++) {
		if (pPathAlias[i] == '\\') {
			pPathAlias[i] = '/';
		}
	}

	pPathAlias[strlen(pPathAlias)] = '\0';

	return pPathAlias;
}
