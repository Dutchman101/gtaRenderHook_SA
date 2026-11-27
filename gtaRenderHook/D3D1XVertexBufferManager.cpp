// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "D3D1XVertexBufferManager.h"


std::list<CD3D1XVertexBuffer*> CD3D1XVertexBufferManager::bufferList = {};

void CD3D1XVertexBufferManager::AddNew( CD3D1XVertexBuffer*& buf )
{
    bufferList.push_back( buf );
}

void CD3D1XVertexBufferManager::Remove( CD3D1XVertexBuffer*& buf )
{
    bufferList.remove( buf );
    delete buf;
    buf = nullptr;
}

void CD3D1XVertexBufferManager::Shutdown()
{
    // Emptied as well as nulled, because this runs once per Stop and again from Close, and a repeated run is allowed.
    // Nulling alone leaves a node per buffer that nothing can reclaim, because Remove looks its entry up by value and no
    // caller passes null. AddNew does not read the entries it walks past, so those nodes cost memory rather than a fault.
    for ( auto &buf : bufferList )
    {
        delete buf;
        buf = nullptr;
    }
    bufferList.clear();
}
