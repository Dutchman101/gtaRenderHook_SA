#pragma once
class CPed;

class CVisibilityPluginsRH
{
public:
    /*!
        Renders weapons without muzzle flash.
    */
    static void RenderWeaponPedsNoMuzzleFlash();
    /*!
        Renders weapons for specific ped without muzzle flash.
    */
    static void RenderWeaponsForPed( CPed* );
    /*!
        Adds ped with weapon to list.
    */
    static void AddPedWithWeapons( CPed* );
    /*!
        Disables muzzle flash rendering.
    */
    static void DisableGunFlashAlpha( CPed * );
    /*!
        Clears ped weapon list.
    */
    static void ClearWeaponPedsList();
    static RpAtomic* RenderAlphaAtomic( RpAtomic *atomic, int fadeAlpha );
    /*!
        Injects methods in memory.
    */
    static void Patch();

private:
    static std::list<CPed*> ms_weaponPeds;
};

