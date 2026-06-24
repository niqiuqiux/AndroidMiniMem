#include "porthelp.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <random>
#include <map>
#include <mutex>
#include "api.h"

struct HANDLE_INFO {
	uint64_t p;
	handleType type;
};
static std::map<HANDLE, HANDLE_INFO> m_HandlePointList;
static std::mutex m_HandleMutex;


HANDLE CPortHelper::CreateHandleFromPointer(uint64_t p, handleType type) {
	std::lock_guard<std::mutex> lock(m_HandleMutex);

	std::random_device rd;
	HANDLE handle = 10000 + rd() / 1000;
	handle = handle < 0 ? -handle : handle;

	while (m_HandlePointList.find(handle) != m_HandlePointList.end()) {
		handle = 10000 + rd() / 1000;
		handle = handle < 0 ? -handle : handle;
	}

	HANDLE_INFO hinfo = { 0 };
	hinfo.p = p;
	hinfo.type = type;
	m_HandlePointList.insert(
		std::pair<HANDLE, HANDLE_INFO >(handle, hinfo));

	return handle;
}

handleType CPortHelper::GetHandleType(HANDLE handle) {
	std::lock_guard<std::mutex> lock(m_HandleMutex);
	auto iter = m_HandlePointList.find(handle);
	if (iter == m_HandlePointList.end()) {
		return htEmpty;
	}
	return iter->second.type;
}

uint64_t CPortHelper::GetPointerFromHandle(HANDLE handle) {
	std::lock_guard<std::mutex> lock(m_HandleMutex);
	auto iter = m_HandlePointList.find(handle);
	if (iter == m_HandlePointList.end()) {
		return 0;
	}
	return iter->second.p;
}



void CPortHelper::RemoveHandle(HANDLE handle) {
	std::lock_guard<std::mutex> lock(m_HandleMutex);
	auto iter = m_HandlePointList.find(handle);
	if (iter == m_HandlePointList.end()) {
		return;
	}
	m_HandlePointList.erase(iter);
}



HANDLE CPortHelper::FindHandleByPID(DWORD pid) {
	std::lock_guard<std::mutex> lock(m_HandleMutex);
	for (auto item = m_HandlePointList.begin(); item != m_HandlePointList.end(); item++) {
		if (item->second.type == htProcesHandle) {
			CeOpenProcess *pCeOpenProcess = (CeOpenProcess*)item->second.p;
			if (pCeOpenProcess->pid == pid) {
				return item->first;
			}
		}
	}
	return 0;
}
