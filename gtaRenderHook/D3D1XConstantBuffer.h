#pragma once
#include <cstring>
#include "D3D1XBuffer.h"
template <class T>
class CD3D1XConstantBuffer :
    public CD3D1XBuffer
{
public:
    CD3D1XConstantBuffer();
    ~CD3D1XConstantBuffer();
    void Update()
    {
        // D3D11 needs a ByteWidth that is a multiple of 16, which AlignedSize provides. When
        // sizeof(T) is already such a multiple the payload is the whole buffer, so the block is
        // handed over as is: the copy through staging exists only to give the padding past
        // sizeof(T) a defined value, and this sits on the per-object and per-material draw path.
        if constexpr ( AlignedSize == sizeof( T ) )
        {
            CD3D1XBuffer::Update( &data );
        }
        else
        {
            unsigned char staging[AlignedSize];
            memcpy( staging, &data, sizeof( T ) );
            memset( staging + sizeof( T ), 0, AlignedSize - sizeof( T ) );
            CD3D1XBuffer::Update( staging, static_cast<int>( AlignedSize ) );
        }
    }
    T    data{};

private:
    static constexpr unsigned int AlignedSize = ( static_cast<unsigned int>( sizeof( T ) ) + 15u ) & ~15u;
};

template<class T>
inline CD3D1XConstantBuffer<T>::CD3D1XConstantBuffer() : CD3D1XBuffer( AlignedSize, D3D11_USAGE_DYNAMIC,
                                                                       D3D11_BIND_CONSTANT_BUFFER,
                                                                       D3D11_CPU_ACCESS_WRITE )
{
    static_assert( AlignedSize <= 65536, "Constant buffer exceeds the D3D11 64 KB byte width limit" );
}

template<class T>
inline CD3D1XConstantBuffer<T>::~CD3D1XConstantBuffer()
{
}
