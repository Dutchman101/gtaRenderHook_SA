// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "RwMethods.h"

void
CameraRotate( RwCamera *camera, const RwV3d *pos, RwReal angle )
{
    RwV3d invCamPos;
    RwFrame *cameraFrame;
    RwMatrix *cameraMatrix;
    RwV3d camPos;

    cameraFrame = RwCameraGetFrame( camera );
    cameraMatrix = RwFrameGetMatrix( cameraFrame );

    camPos = ( pos ) ? *pos : *RwMatrixGetPos( cameraMatrix );

    RwV3dScale( &invCamPos, &camPos, -1.0f );

    /*
    * Translate the camera back to the rotation origin...
    */
    RwFrameTranslate( cameraFrame, &invCamPos, rwCOMBINEPOSTCONCAT );

    /*
    * Get the camera's AT vector and use this as the axis of rotation...
    */
    RwMatrixRotate( cameraMatrix, RwMatrixGetRight( cameraMatrix ),
                    angle, rwCOMBINEPOSTCONCAT );

    /*
    * Translate the camera back to its original position...
    */
    RwFrameTranslate( cameraFrame, &camPos, rwCOMBINEPOSTCONCAT );

    return;
}
void
CameraRotateAroundAxis( RwCamera *camera, const RwV3d *axis, const RwV3d *pos, RwReal angle )
{
    RwV3d invCamPos;
    RwFrame *cameraFrame;
    RwMatrix *cameraMatrix;
    RwV3d camPos;

    cameraFrame = RwCameraGetFrame( camera );
    cameraMatrix = RwFrameGetMatrix( cameraFrame );

    camPos = ( pos ) ? *pos : *RwMatrixGetPos( cameraMatrix );

    RwV3dScale( &invCamPos, &camPos, -1.0f );

    RwFrameTranslate( cameraFrame, &invCamPos, rwCOMBINEPOSTCONCAT );

    RwMatrixRotate( cameraMatrix, axis, angle, rwCOMBINEPOSTCONCAT );

    RwFrameTranslate( cameraFrame, &camPos, rwCOMBINEPOSTCONCAT );

    return;
}
void
CameraRotate2( RwCamera *camera, const RwV3d *pos, RwReal angle )
{
    RwV3d invCamPos;
    RwFrame *cameraFrame;
    RwMatrix *cameraMatrix;
    RwV3d camPos;

    cameraFrame = RwCameraGetFrame( camera );
    cameraMatrix = RwFrameGetMatrix( cameraFrame );

    camPos = ( pos ) ? *pos : *RwMatrixGetPos( cameraMatrix );

    RwV3dScale( &invCamPos, &camPos, -1.0f );

    /*
    * Translate the camera back to the rotation origin...
    */
    RwFrameTranslate( cameraFrame, &invCamPos, rwCOMBINEPOSTCONCAT );

    /*
    * Get the camera's AT vector and use this as the axis of rotation...
    */
    RwMatrixRotate( cameraMatrix, RwMatrixGetUp( cameraMatrix ),
                    angle, rwCOMBINEPOSTCONCAT );

    /*
    * Translate the camera back to its original position...
    */
    RwFrameTranslate( cameraFrame, &camPos, rwCOMBINEPOSTCONCAT );

    return;
}

float rwV3D_Dist( const RwV3d &a, const RwV3d &b )
{
    float
        dX = b.x - a.x,
        dY = b.y - a.y,
        dZ = b.z - a.z;

    return sqrt( dX*dX + dY * dY + dZ * dZ );
}
RwV3d rwV3D_Normalize( const RwV3d & a )
{
    float length = sqrt( a.x*a.x + a.y*a.y + a.z*a.z );
    // A zero-length or non-finite vector has no direction, so the input is handed back instead of being
    // divided by; the ">" test is false for NaN as well. A component large enough to overflow the squared
    // sum produces an infinite length, which the second test rejects: dividing by it would return a zero
    // vector rather than the input. V3d::normalize and V4d::normalize leave the same cases alone, so the
    // three behave alike.
    if ( length > 0.0f && length < std::numeric_limits<float>::infinity() )
        return { a.x / length ,a.y / length,a.z / length };
    return a;
}
RwV3d rwV3D_Cross( const RwV3d & a, const RwV3d & b )
{
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z,     a.x*b.y - a.y*b.x };
}
float rwV3D_Dot( const RwV3d & a, const RwV3d & b )
{
    return { a.x*b.x + a.y*b.y + a.z*b.z };
}

RwV3d rwV3D_Max( const RwV3d & a, const RwV3d & b )
{
    return { max( a.x, b.x ), max( a.y, b.y ), max( a.z, b.z ) };
}
RwV3d rwV3D_Min( const RwV3d & a, const RwV3d & b )
{
    return { min( a.x, b.x ), min( a.y, b.y ), min( a.z, b.z ) };
}
