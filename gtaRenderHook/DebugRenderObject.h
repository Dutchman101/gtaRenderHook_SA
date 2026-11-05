#pragma once
/*!
    \class DebugRenderObject

    Base class for rendering debug objects.
*/
class DebugRenderObject
{
public:
    DebugRenderObject();
    // Virtual because the debug list owns derived objects and releases them through this type.
    virtual ~DebugRenderObject();
    virtual void Render() = 0;
};

