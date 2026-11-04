
#ifndef stdafx_h__
#define stdafx_h__

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define NOASM
#define USE_ANTTWEAKBAR

#pragma warning( push, 0 )
// Windows Header Files:
#include <windows.h>
#include <stdio.h>
#include <string>
#include <fstream>
#include <thread>
#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <limits>
#include <array>
#include <list>
// Render API Header Files:
#include <d3d9.h>
#include <d3d11_3.h>
#include <DirectXMath.h>
#include <atlbase.h>

//#define DebuggingShaders
#pragma comment( lib, "dxguid.lib")
//#define _WIN32_WINNT 0x600

#pragma warning( pop )

#include "gta_sa_ptrs.h"
#include "MemoryFuncs.h"

#endif // stdafx_h
