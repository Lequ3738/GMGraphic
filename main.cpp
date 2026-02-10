// GMAPI setup

#include "main.h"

gm::CGMAPI* gmapi;

bool WINAPI DllMain(HINSTANCE aModuleHandle, int aReason, int aReserved)
{
	switch (aReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			ulong result = 0;
			gmapi = gm::CGMAPI::Create(&result);
			// Check the initialization
			if (result == gm::GMAPI_INITIALIZATION_FAILED)
			{
				MessageBox(NULL, L"Unable to initialize GMAPI.", NULL, MB_SYSTEMMODAL | MB_ICONERROR);
				return FALSE;
			}
		}
		break;

		case DLL_PROCESS_DETACH:
		{
			gmapi->Destroy();
		}
		break;
	}

	return true;
}