/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal ELF64 -> PE32+ converter for EFI applications.
 * - Merges all PT_LOAD segments into a single flat PE section
 * - No relocations emitted (fixed base)
 * - BSS (memsz > filesz) excluded from PE image; kernel handles it.
 */

#define EI_NIDENT 16
#define PT_LOAD 1

#define PE_FILE_ALIGNMENT 0x200
#define PE_SECTION_ALIGNMENT 0x1000

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550

#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020

#define IMAGE_SUBSYSTEM_EFI_APPLICATION 10

#define IMAGE_SCN_CNT_CODE 0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

typedef struct {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} Elf64_Phdr;

typedef struct {
	uint16_t e_magic;
	uint16_t e_cblp;
	uint16_t e_cp;
	uint16_t e_crlc;
	uint16_t e_cparhdr;
	uint16_t e_minalloc;
	uint16_t e_maxalloc;
	uint16_t e_ss;
	uint16_t e_sp;
	uint16_t e_csum;
	uint16_t e_ip;
	uint16_t e_cs;
	uint16_t e_lfarlc;
	uint16_t e_ovno;
	uint16_t e_res[4];
	uint16_t e_oemid;
	uint16_t e_oeminfo;
	uint16_t e_res2[10];
	uint32_t e_lfanew;
} DosHeader;

typedef struct {
	uint16_t Machine;
	uint16_t NumberOfSections;
	uint32_t TimeDateStamp;
	uint32_t PointerToSymbolTable;
	uint32_t NumberOfSymbols;
	uint16_t SizeOfOptionalHeader;
	uint16_t Characteristics;
} CoffHeader;

typedef struct {
	uint16_t Magic;
	uint8_t MajorLinkerVersion;
	uint8_t MinorLinkerVersion;
	uint32_t SizeOfCode;
	uint32_t SizeOfInitializedData;
	uint32_t SizeOfUninitializedData;
	uint32_t AddressOfEntryPoint;
	uint32_t BaseOfCode;
	uint64_t ImageBase;
	uint32_t SectionAlignment;
	uint32_t FileAlignment;
	uint16_t MajorOperatingSystemVersion;
	uint16_t MinorOperatingSystemVersion;
	uint16_t MajorImageVersion;
	uint16_t MinorImageVersion;
	uint16_t MajorSubsystemVersion;
	uint16_t MinorSubsystemVersion;
	uint32_t Win32VersionValue;
	uint32_t SizeOfImage;
	uint32_t SizeOfHeaders;
	uint32_t CheckSum;
	uint16_t Subsystem;
	uint16_t DllCharacteristics;
	uint64_t SizeOfStackReserve;
	uint64_t SizeOfStackCommit;
	uint64_t SizeOfHeapReserve;
	uint64_t SizeOfHeapCommit;
	uint32_t LoaderFlags;
	uint32_t NumberOfRvaAndSizes;
	struct {
		uint32_t VirtualAddress;
		uint32_t Size;
	} DataDirectory[16];
} OptionalHeader64;

typedef struct {
	char Name[8];
	uint32_t VirtualSize;
	uint32_t VirtualAddress;
	uint32_t SizeOfRawData;
	uint32_t PointerToRawData;
	uint32_t PointerToRelocations;
	uint32_t PointerToLinenumbers;
	uint16_t NumberOfRelocations;
	uint16_t NumberOfLinenumbers;
	uint32_t Characteristics;
} SectionHeader;

static uint32_t
align_u32(uint32_t v, uint32_t a)
{
	return (v + a - 1u) & ~(a - 1u);
}

static void
die(const char *msg)
{
	fprintf(stderr, "elf2efi: %s\n", msg);
	exit(1);
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <in.elf> <out.efi>\n", argv[0]);
		return 2;
	}

	FILE *fi = fopen(argv[1], "rb");
	if (!fi)
		die("failed to open input");

	Elf64_Ehdr eh;
	if (fread(&eh, 1, sizeof(eh), fi) != sizeof(eh))
		die("read ELF header failed");
	if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E'
	    || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
		die("not an ELF file");
	if (eh.e_ident[4] != 2)
		die("not ELF64");
	if (eh.e_machine != 0x3E)
		die("not x86_64");
	if (eh.e_phnum == 0)
		die("no program headers");

	if (fseek(fi, (long)eh.e_phoff, SEEK_SET) != 0)
		die("seek phoff failed");

	/* Collect PT_LOAD segments. */
	Elf64_Phdr loads[16];
	uint16_t nloads = 0;
	uint64_t lowest_paddr = 0xFFFFFFFFFFFFFFFFULL;
	uint64_t highest_file_end = 0;

	for (uint16_t i = 0; i < eh.e_phnum; i++) {
		Elf64_Phdr ph;
		if (fread(&ph, 1, sizeof(ph), fi) != sizeof(ph))
			die("read phdr failed");
		if (ph.p_type != PT_LOAD)
			continue;
		if (ph.p_filesz == 0)
			continue;
		if (nloads >= 16)
			die("too many PT_LOAD segments");

		loads[nloads++] = ph;

		if (ph.p_paddr < lowest_paddr)
			lowest_paddr = ph.p_paddr;
		if (ph.p_paddr + ph.p_filesz > highest_file_end)
			highest_file_end = ph.p_paddr + ph.p_filesz;
	}

	if (nloads == 0)
		die("no loadable segments with file data");

	/*
	 * Compute ImageBase so that image_base + PE_SECTION_ALIGNMENT == lowest_paddr.
	 * This guarantees sec_rva is always exactly one page, avoiding overlap with
	 * PE headers, while preserving the ELF virtual address layout in the PE.
	 */
	uint64_t image_base = lowest_paddr - PE_SECTION_ALIGNMENT;
	uint64_t flat_size = highest_file_end - lowest_paddr;

	/* Build flat image: all segments merged into one buffer. */
	uint8_t *flat = (uint8_t *) malloc((size_t) flat_size);
	if (!flat)
		die("alloc flat buffer failed");
	memset(flat, 0, (size_t) flat_size);

	for (uint16_t i = 0; i < nloads; i++) {
		uint64_t rel = loads[i].p_paddr - lowest_paddr;
		if (fseek(fi, (long)loads[i].p_offset, SEEK_SET) != 0)
			die("seek segment failed");
		if (fread(flat + rel, 1, (size_t) loads[i].p_filesz, fi) !=
		    loads[i].p_filesz)
			die("read segment failed");
	}

	/* PE layout: 1 section at RVA = PE_SECTION_ALIGNMENT. */
	uint32_t sec_rva = PE_SECTION_ALIGNMENT;
	uint32_t sec_rawsize =
	    align_u32((uint32_t) flat_size, PE_FILE_ALIGNMENT);
	uint32_t sec_vsize = (uint32_t) flat_size;
	uint32_t entry_rva = sec_rva + (uint32_t) (eh.e_entry - lowest_paddr);

	/* Headers: DOS(64) + stub padding + PE sig(4) + COFF(20) + OptHdr(240) + 1 SectionHdr(40) */
	uint32_t pe_header_offset = 0x80;
	uint32_t headers_size =
	    pe_header_offset + 4 + sizeof(CoffHeader) +
	    sizeof(OptionalHeader64) + sizeof(SectionHeader);
	uint32_t size_of_headers = align_u32(headers_size, PE_FILE_ALIGNMENT);
	uint32_t size_of_image =
	    align_u32(sec_rva + align_u32(sec_vsize, PE_SECTION_ALIGNMENT),
		      PE_SECTION_ALIGNMENT);

	FILE *fo = fopen(argv[2], "wb");
	if (!fo)
		die("failed to open output");

	/* DOS header. */
	DosHeader dos;
	memset(&dos, 0, sizeof(dos));
	dos.e_magic = IMAGE_DOS_SIGNATURE;
	dos.e_lfanew = pe_header_offset;
	fwrite(&dos, 1, sizeof(dos), fo);

	/* Pad to pe_header_offset. */
	{
		long pos = ftell(fo);
		while (pos < (long)pe_header_offset) {
			fputc(0, fo);
			pos++;
		}
	}

	/* PE signature. */
	uint32_t sig = IMAGE_NT_SIGNATURE;
	fwrite(&sig, 1, sizeof(sig), fo);

	/* COFF header. */
	CoffHeader coff;
	memset(&coff, 0, sizeof(coff));
	coff.Machine = IMAGE_FILE_MACHINE_AMD64;
	coff.NumberOfSections = 1;
	coff.SizeOfOptionalHeader = sizeof(OptionalHeader64);
	coff.Characteristics =
	    IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
	fwrite(&coff, 1, sizeof(coff), fo);

	/* Optional header. */
	OptionalHeader64 opt;
	memset(&opt, 0, sizeof(opt));
	opt.Magic = 0x20B;	/* PE32+. */
	opt.SizeOfCode = sec_rawsize;
	opt.AddressOfEntryPoint = entry_rva;
	opt.BaseOfCode = sec_rva;
	opt.ImageBase = image_base;
	opt.SectionAlignment = PE_SECTION_ALIGNMENT;
	opt.FileAlignment = PE_FILE_ALIGNMENT;
	opt.MajorOperatingSystemVersion = 1;
	opt.MajorSubsystemVersion = 1;
	opt.SizeOfImage = size_of_image;
	opt.SizeOfHeaders = size_of_headers;
	opt.Subsystem = IMAGE_SUBSYSTEM_EFI_APPLICATION;
	opt.SizeOfStackReserve = 0x40000;
	opt.SizeOfStackCommit = 0x4000;
	opt.SizeOfHeapReserve = 0x40000;
	opt.SizeOfHeapCommit = 0x4000;
	opt.NumberOfRvaAndSizes = 16;
	fwrite(&opt, 1, sizeof(opt), fo);

	/* Section header. */
	SectionHeader sh;
	memset(&sh, 0, sizeof(sh));
	memcpy(sh.Name, ".flat", 5);
	sh.VirtualSize = sec_vsize;
	sh.VirtualAddress = sec_rva;
	sh.SizeOfRawData = sec_rawsize;
	sh.PointerToRawData = size_of_headers;
	sh.Characteristics =
	    IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
	    IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
	fwrite(&sh, 1, sizeof(sh), fo);

	/* Pad to SizeOfHeaders. */
	{
		long pos = ftell(fo);
		while (pos < (long)size_of_headers) {
			fputc(0, fo);
			pos++;
		}
	}

	/* Write flat section data. */
	fwrite(flat, 1, (size_t) flat_size, fo);

	/* Pad to SizeOfRawData. */
	{
		uint32_t pad = sec_rawsize - (uint32_t) flat_size;
		for (uint32_t i = 0; i < pad; i++)
			fputc(0, fo);
	}

	fclose(fi);
	fclose(fo);
	free(flat);

	fprintf(stderr, "elf2efi: %s -> %s\n", argv[1], argv[2]);

	return 0;
}
