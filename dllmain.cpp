// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <windows.h>
#include <stdio.h>
#include <iostream>
#include <conio.h>
#include <string>
#include <fstream>
#include <vector>
#include <float.h>
#include "umvc3utils.h"
#include "MemoryMgr.h"
#include <TlHelp32.h> 
#include <memory.h>

//Some of this is copied pasted from Color Expansion in case something comes up later and I have to expand.
using namespace Memory::VP;

#define longlong  long long
#define ulonglong  unsigned long long
#define undefined8  long long*
#define undefined7  long long
#define undefined2  int
typedef unsigned __int64 QWORD;
typedef int(*code)(longlong* param_1);
typedef int(*codenoarg)();
typedef void(*method)(void);
#define undefined int
#define undefined4 int
#define CONCAT71(a,b) (a<<32 | b)
#define backupsize 10000//100000
#define prebackup 0 //100000

HANDLE hProcess = 0;
uintptr_t moduleBase = 0;
size_t ProjectedModuleSize = 0xEBB000;
DWORD oldPageProtectionPainSounds = 0;

#pragma region AOB strings

//Thanks to rain for showing me where this is.
const char* AOBPainSounds = "40 8B 43 04 89 44 24 44 E8 ?? ?? ?? ?? B8 E8 03 00 00 0F 28 F0 66 39 43 ?? ?? ?? 48 83 CA FF 48";

#pragma endregion

//Gotta check the game version somehow, right?
bool CheckGame()
{
    char* gameName = (char*)(_addr(0x140B12D10));

    if (strcmp(gameName, "umvc3") == 0)
    {
        return true;
    }
    else
    {
        MessageBoxA(0, "Sorry!\nThis only supports the most recent(or the version released from April 2017 - Somewhere in 2026) Steam Executable of Ultimate Marvel vs Capcom 3.", 0, MB_ICONINFORMATION);
        return false;
    }
}

#pragma region ProtectedWrites

DWORD SingleProtectedWriteByte(DWORD64 AddressToWrite, int length, BYTE ByteToWrite, DWORD oldPageProtection)
{
	oldPageProtection = 0;

	VirtualProtect((LPVOID)AddressToWrite, 1, PAGE_EXECUTE_READWRITE, &oldPageProtection);

	*((std::byte*)(AddressToWrite)) = (std::byte)ByteToWrite;

	VirtualProtect((LPVOID)AddressToWrite, 1, oldPageProtection, &oldPageProtection);
	return 0;
}

DWORD MultiProtectedWriteShort(DWORD64 AddressToWrite, int length, short ValuToWrite, DWORD oldPageProtection)
{
	DWORD errorMessageID = 0;
	LPSTR messageBuffer = nullptr;
	oldPageProtection = 0;

	VirtualProtect((LPVOID)AddressToWrite, 1, PAGE_EXECUTE_READWRITE, &oldPageProtection);

	if (!(*((short*)(AddressToWrite)) = ValuToWrite))
	{
		errorMessageID = ::GetLastError();
		messageBuffer = nullptr;

		std::cout << "Nope! For some reason, cannot write to " << std::hex << AddressToWrite;
		std::cout << "\nBecause of: " << messageBuffer << std::endl;
	}

	VirtualProtect((LPVOID)AddressToWrite, 1, oldPageProtection, &oldPageProtection);
	return 0;
}

DWORD MultiProtectedWriteByteArray(DWORD64 AddressToWrite, int length, BYTE BytesToWrite[], DWORD oldPageProtection)
{
	DWORD errorMessageID = 0;
	LPSTR messageBuffer = nullptr;

	oldPageProtection = 0;

	VirtualProtect((LPVOID)AddressToWrite, length, PAGE_EXECUTE_READWRITE, &oldPageProtection);

	DWORD64 TempAddr = AddressToWrite;
	int TempLength = length - 1;
	for (int i = 0; i < length; i++)
	{

		*((std::byte*)(TempAddr)) = (std::byte)BytesToWrite[i];
		TempAddr++;

	}

	/*
	if(!(*((LPVOID*)(AddressToWrite)) = BytesToWrite))
	{
		errorMessageID = ::GetLastError();
		messageBuffer = nullptr;

		std::cout << "Nope! For some reason, cannot write to " << std::hex << AddressToWrite;
		std::cout << "\nBecause of: " << messageBuffer << std::endl;
	}
	else
	{
		std::cout << "Succesfully wrote to " << std::hex << AddressToWrite << std::endl;
	}
	*/

	VirtualProtect((LPVOID)AddressToWrite, length, oldPageProtection, &oldPageProtection);
	return 0;
}

#pragma endregion

//Originally based on HeathHowren's PatterScanning function. Adjusted to better suit the target game, which is x64.

static inline uint8_t ReadHexNibble(char c) {
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	c &= ~0x20;
	if (c >= 'A' && c <= 'F')
	{
		return c - 'A' + 0xA;
	}
	return 0;
}

static inline bool ParseNextCharacter(const char*& pat, uint8_t& outByte) {

	//Skip leading spaces.
	while (*pat == ' ') ++pat;

	if (!*pat)
	{
		//The pattern's end.
		return false;
	}

	bool wild = (pat[0] == '?');

	if (!wild)
	{
		outByte = (ReadHexNibble(pat[0]) << 4) | ReadHexNibble(pat[1]);
	}

	//Advances a character and past any trailing spaces.
	if (pat[0] == '?' && pat[1] != '?' && pat[1] != ' ' && pat[1] != '\0')
	{
		pat += 1;
	}
	else if (pat[0] == '?' && (pat[1] == '?'))
	{
		pat += 2;
	}
	else if (pat[0] == '?')
	{
		pat += 1;
	}
	else
	{
		pat += 2;
	}

	while (*pat == ' ')
	{
		//Skip whatever trailing spaces remain.
		++pat;
	}
	return wild;
}

uint64_t FindBytePattern(uint64_t baseAddr, size_t ScanSize, const char* DataPattern)
{
	//Some checking of validity.
	if (baseAddr <= 0 || ScanSize <= 0 || !DataPattern)
	{
		return 0;
	}

	const char* CPattern = DataPattern;
	uint64_t    FirstMatchLocation = 0;

	for (uint64_t pCur = baseAddr; pCur < (baseAddr + ScanSize); ++pCur)
	{
		while (*CPattern == ' ')
		{
			++CPattern;
		}
		if (!*CPattern)
		{
			break;
		}

		uint8_t    patByte = 0;
		const char* patSave = CPattern;
		bool wild = ParseNextCharacter(CPattern, patByte);
		uint8_t curByte = *reinterpret_cast<uint8_t*>(pCur);

		if (wild || curByte == patByte)
		{
			if (!FirstMatchLocation) FirstMatchLocation = pCur;
			{
				if (!*CPattern)
				{
					break;
				}
			}
		}
		else {
			//Resets on mismatch.
			CPattern = DataPattern;
			FirstMatchLocation = 0;
		}
	}


	if (!FirstMatchLocation)
	{
		return 0;
	}

	return FirstMatchLocation;

}

void OnInitializeHook()
{
	//First off the module base.
	moduleBase = (uintptr_t)GetModuleHandle(L"umvc3.exe");

	//Time to start changing that 1000 to 2000 to allow the pain sounds to play properly.
	BYTE ForPainSounds[] = {0xD0,0x07};

	//The pain sounds pointer. The pain sound range is greater than 1000 and for some reason there was a check for +1000 ranges added in the PS4 era ports...
	uint64_t PainSoundsPtr = 0xE + FindBytePattern(moduleBase, ProjectedModuleSize, AOBPainSounds);
	MultiProtectedWriteByteArray(PainSoundsPtr, 2, ForPainSounds, oldPageProtectionPainSounds);



}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		if (CheckGame())
		{
			CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)OnInitializeHook, hModule, 0, nullptr);
		}
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

