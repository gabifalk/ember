/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * ember-ld: Minimal static ELF64 linker.
 *
 * - Merges .text, .rodata, .data, .bss sections from ELF64 relocatable objects
 * - Resolves all relocations statically (no PLT, no GOT for function calls)
 * - Builds a small GOT only for R_X86_64_GOTPCREL variable references
 * - Emits a static ELF64 executable
 * - Supports separate VMA/LMA for higher-half kernel linking
 *
 * Compilable by TCC or GCC.  No external dependencies beyond libc.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ELF64 definitions (self-contained) ---- */

#define EI_NIDENT 16

typedef struct {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum;
	uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t sh_name, sh_type;
	uint64_t sh_flags, sh_addr, sh_offset, sh_size;
	uint32_t sh_link, sh_info;
	uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
	uint32_t st_name;
	uint8_t st_info, st_other;
	uint16_t st_shndx;
	uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct {
	uint64_t r_offset;
	uint64_t r_info;
	int64_t r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)   ((uint32_t)((i) & 0xffffffff))
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)

/* ELF constants. */
#define ET_REL    1
#define ET_EXEC   2
#define EM_X86_64 62
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHN_UNDEF    0
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define PT_LOAD    1
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Relocation types. */
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_PLT32     4
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32       10
#define R_X86_64_32S      11

/* ---- Data structures ---- */

#define MAX_OBJS     512
#define MAX_ISECS    8192
#define MAX_SYMS     32768
#define MAX_GOT      1024

enum { SECT_TEXT, SECT_RODATA, SECT_DATA, SECT_BSS, SECT_NKINDS };

typedef struct ObjFile {
	const char *path;
	uint8_t *data;
	size_t size;
	Elf64_Ehdr *ehdr;
	Elf64_Shdr *shdrs;
	const char *shstrtab;
	Elf64_Sym *symtab;
	int nsyms;
	const char *strtab;
	int *sec_map;		/* sec_map[shndx] = index into g.isecs, or -1. */
	int *sym_map;		/* sym_map[sym_idx] = index into g.syms, or -1. */
} ObjFile;

typedef struct {
	ObjFile *obj;
	int shndx;
	int kind;		/* SECT_TEXT, SECT_RODATA, SECT_DATA, SECT_BSS. */
	uint64_t out_offset;	/* Offset within merged section of this kind. */
	uint64_t size;
	uint64_t align;
	uint8_t *data;		/* NULL for BSS. */
} InputSection;

typedef struct {
	const char *name;
	uint64_t value;		/* Final VMA after layout. */
	int isec_idx;		/* Index into g.isecs, or -1 for ABS. */
	uint64_t sec_offset;	/* Offset within input section. */
	int binding;
	int defined;		/* 1 If defined, 0 if only referenced. */
} Symbol;

typedef struct {
	int sym_idx;		/* Index into g.syms. */
	uint64_t got_offset;	/* Offset within .got section. */
} GotEntry;

static struct {
	ObjFile objs[MAX_OBJS];
	int nobjs;

	InputSection isecs[MAX_ISECS];
	int nisecs;

	Symbol syms[MAX_SYMS];
	int nsyms;

	GotEntry got[MAX_GOT];
	int ngot;

	/* Output merged-section layout. */
	struct {
		uint64_t vma;
		uint64_t file_off;
		uint64_t size;
	} merged[SECT_NKINDS];

	uint64_t got_vma;
	uint64_t got_file_off;
	uint64_t got_size;

	uint64_t vma_base;
	uint64_t lma_base;
	int have_lma;		/* 1 If --lma was given. */
} g;

/* ---- Utilities ---- */

static void
die(const char *msg)
{
	fprintf(stderr, "ember-ld: %s\n", msg);
	exit(1);
}

static void
dief(const char *fmt, const char *arg)
{
	fprintf(stderr, "ember-ld: ");
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n");
	exit(1);
}

static uint64_t
align_up(uint64_t v, uint64_t a)
{
	if (a == 0)
		a = 1;
	return (v + a - 1) & ~(a - 1);
}

static void
read_file(const char *path, uint8_t ** out, size_t * out_size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		dief("cannot open: %s", path);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = (uint8_t *) malloc((size_t) sz);
	if (!buf)
		die("malloc failed");
	if ((long)fread(buf, 1, (size_t) sz, f) != sz)
		dief("read error: %s", path);
	fclose(f);
	*out = buf;
	*out_size = (size_t) sz;
}

/* ---- ELF object parser ---- */

static int
classify_section(const char *name)
{
	if (strcmp(name, ".text") == 0)
		return SECT_TEXT;
	if (strncmp(name, ".text.", 6) == 0)
		return SECT_TEXT;
	if (strcmp(name, ".rodata") == 0)
		return SECT_RODATA;
	if (strncmp(name, ".rodata.", 8) == 0)
		return SECT_RODATA;
	if (strcmp(name, ".data") == 0)
		return SECT_DATA;
	if (strncmp(name, ".data.", 6) == 0)
		return SECT_DATA;
	if (strcmp(name, ".bss") == 0)
		return SECT_BSS;
	if (strncmp(name, ".bss.", 5) == 0)
		return SECT_BSS;
	return -1;		/* Discard. */
}

static void
load_obj(const char *path)
{
	if (g.nobjs >= MAX_OBJS)
		die("too many objects");
	ObjFile *o = &g.objs[g.nobjs++];
	o->path = path;
	read_file(path, &o->data, &o->size);

	o->ehdr = (Elf64_Ehdr *) o->data;
	if (o->size < sizeof(Elf64_Ehdr))
		dief("truncated: %s", path);
	if (o->ehdr->e_ident[0] != 0x7f || o->ehdr->e_ident[1] != 'E' ||
	    o->ehdr->e_ident[2] != 'L' || o->ehdr->e_ident[3] != 'F')
		dief("not ELF: %s", path);
	if (o->ehdr->e_ident[4] != 2)
		dief("not ELF64: %s", path);
	if (o->ehdr->e_type != ET_REL)
		dief("not relocatable: %s", path);
	if (o->ehdr->e_machine != EM_X86_64)
		dief("not x86_64: %s", path);

	o->shdrs = (Elf64_Shdr *) (o->data + o->ehdr->e_shoff);
	o->shstrtab =
	    (const char *)(o->data + o->shdrs[o->ehdr->e_shstrndx].sh_offset);
	o->symtab = NULL;
	o->nsyms = 0;
	o->strtab = NULL;

	int num_shdrs = o->ehdr->e_shnum;
	o->sec_map = (int *)calloc((size_t) num_shdrs, sizeof(int));
	for (int i = 0; i < num_shdrs; i++)
		o->sec_map[i] = -1;

	/* Find symtab and strtab; collect sections. */
	for (int i = 0; i < num_shdrs; i++) {
		Elf64_Shdr *sh = &o->shdrs[i];
		if (sh->sh_type == SHT_SYMTAB) {
			o->symtab = (Elf64_Sym *) (o->data + sh->sh_offset);
			o->nsyms = (int)(sh->sh_size / sizeof(Elf64_Sym));
			/* Strtab is linked via sh_link. */
			Elf64_Shdr *str_sh = &o->shdrs[sh->sh_link];
			o->strtab = (const char *)(o->data + str_sh->sh_offset);
		}
	}

	/* Classify and collect input sections. */
	for (int i = 0; i < num_shdrs; i++) {
		Elf64_Shdr *sh = &o->shdrs[i];
		if (sh->sh_type != SHT_PROGBITS && sh->sh_type != SHT_NOBITS)
			continue;
		const char *name = o->shstrtab + sh->sh_name;
		int kind = classify_section(name);
		if (kind < 0)
			continue;
		/* SHT_NOBITS -> BSS regardless of name. */
		if (sh->sh_type == SHT_NOBITS)
			kind = SECT_BSS;

		if (g.nisecs >= MAX_ISECS)
			die("too many sections");
		int idx = g.nisecs++;
		InputSection *is = &g.isecs[idx];
		is->obj = o;
		is->shndx = i;
		is->kind = kind;
		is->size = sh->sh_size;
		is->align = sh->sh_addralign;
		if (is->align == 0)
			is->align = 1;
		is->data =
		    (sh->sh_type ==
		     SHT_NOBITS) ? NULL : (o->data + sh->sh_offset);
		is->out_offset = 0;
		o->sec_map[i] = idx;
	}
}

/* ---- Thin archive parser ---- */

static void
load_archive(const char *path)
{
	uint8_t *data;
	size_t size;
	read_file(path, &data, &size);

	if (size < 8 || memcmp(data, "!<thin>\n", 8) != 0)
		dief("not a thin archive: %s", path);

	const char *name_tab = NULL;
	size_t name_tab_size = 0;
	size_t pos = 8;

	/* First pass: find extended name table. */
	while (pos + 60 <= size) {
		char *hdr = (char *)(data + pos);
		if (hdr[58] != '`' || hdr[59] != '\n')
			break;
		long member_size = strtol(hdr + 48, NULL, 10);
		size_t padded = (size_t) ((member_size + 1) & ~1L);

		int is_special = (hdr[0] == '/'
				  && (hdr[1] == '/' || hdr[1] == ' '));
		if (hdr[0] == '/' && hdr[1] == '/') {
			name_tab = (const char *)(data + pos + 60);
			name_tab_size = (size_t) member_size;
		}
		/* Only special entries have inline data in thin archives. */
		pos += 60 + (is_special ? padded : 0);
	}

	/*
	 * Second pass: load members.
	 * In thin archives, only special entries (/ and //) have inline data.
	 * Regular member entries are headers only -- file data is external.
	 */
	pos = 8;
	while (pos + 60 <= size) {
		char *hdr = (char *)(data + pos);
		if (hdr[58] != '`' || hdr[59] != '\n')
			break;
		long member_size = strtol(hdr + 48, NULL, 10);
		size_t padded = (size_t) ((member_size + 1) & ~1L);

		int is_special = (hdr[0] == '/'
				  && (hdr[1] == '/' || hdr[1] == ' '));
		if (is_special) {
			/* Special entries have inline data. */
			pos += 60 + padded;
			continue;
		}

		/* Regular member: extract name, no inline data. */
		char name[4096];
		if (hdr[0] == '/') {
			/*
			 * Long name: /offset into name table.
			 * Thin archive names are terminated by "/\n".
			 */
			long off = strtol(hdr + 1, NULL, 10);
			if (!name_tab || off < 0
			    || (size_t) off >= name_tab_size)
				die("bad archive name offset");
			int j = 0;
			for (size_t i = (size_t) off; i < name_tab_size; i++) {
				if (name_tab[i] == '\n')
					break;
				/* In thin archives, '/' before '\n' is the terminator. */
				if (name_tab[i] == '/' && i + 1 < name_tab_size
				    && name_tab[i + 1] == '\n')
					break;
				name[j++] = name_tab[i];
			}
			name[j] = '\0';
		} else {
			/* Short name: terminated with '/'. */
			int j = 0;
			for (int i = 0;
			     i < 16 && hdr[i] != '/' && hdr[i] != ' '; i++)
				name[j++] = hdr[i];
			name[j] = '\0';
		}

		if (name[0] != '\0') {
			char *dup = (char *)malloc(strlen(name) + 1);
			strcpy(dup, name);
			load_obj(dup);
		}

		pos += 60;	/* Thin archive: no inline data for members. */
	}

	free(data);
}

/* ---- Symbol resolution ---- */

/* Simple hash for symbol lookup. */
#define SYM_HASH_SIZE 4096
static int sym_hash[SYM_HASH_SIZE];	/* Index into g.syms, or -1. */
static int sym_hash_next[MAX_SYMS];	/* Chaining. */

static void
sym_hash_init(void)
{
	for (int i = 0; i < SYM_HASH_SIZE; i++)
		sym_hash[i] = -1;
}

static unsigned
sym_hash_fn(const char *s)
{
	unsigned h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h % SYM_HASH_SIZE;
}

static int
sym_find(const char *name)
{
	unsigned h = sym_hash_fn(name);
	for (int i = sym_hash[h]; i >= 0; i = sym_hash_next[i])
		if (strcmp(g.syms[i].name, name) == 0)
			return i;
	return -1;
}

static int
sym_add(const char *name)
{
	if (g.nsyms >= MAX_SYMS)
		die("too many symbols");
	int idx = g.nsyms++;
	g.syms[idx].name = name;
	g.syms[idx].value = 0;
	g.syms[idx].isec_idx = -1;
	g.syms[idx].sec_offset = 0;
	g.syms[idx].binding = STB_GLOBAL;
	g.syms[idx].defined = 0;
	unsigned h = sym_hash_fn(name);
	sym_hash_next[idx] = sym_hash[h];
	sym_hash[h] = idx;
	return idx;
}

static void
resolve_symbols(void)
{
	sym_hash_init();

	for (int oi = 0; oi < g.nobjs; oi++) {
		ObjFile *o = &g.objs[oi];
		if (!o->symtab)
			continue;

		o->sym_map = (int *)calloc((size_t) o->nsyms, sizeof(int));
		for (int i = 0; i < o->nsyms; i++)
			o->sym_map[i] = -1;

		for (int si = 0; si < o->nsyms; si++) {
			Elf64_Sym *es = &o->symtab[si];
			int bind = ELF64_ST_BIND(es->st_info);
			int type = ELF64_ST_TYPE(es->st_info);
			(void)type;

			if (si == 0)
				continue;	/* Skip null symbol. */

			const char *name = o->strtab + es->st_name;

			if (bind == STB_LOCAL) {
				/* Local symbol -- create a private entry. */
				if (es->st_shndx == SHN_UNDEF)
					continue;
				if (es->st_shndx >= o->ehdr->e_shnum)
					continue;

				int isec = o->sec_map[es->st_shndx];
				if (isec < 0)
					continue;	/* In discarded section. */

				if (g.nsyms >= MAX_SYMS)
					die("too many symbols");
				int idx = g.nsyms++;
				g.syms[idx].name = name;
				g.syms[idx].isec_idx = isec;
				g.syms[idx].sec_offset = es->st_value;
				g.syms[idx].binding = STB_LOCAL;
				g.syms[idx].defined = 1;
				/* Don't add to hash -- locals are per-object. */
				o->sym_map[si] = idx;
				continue;
			}

			/* GLOBAL or WEAK. */
			if (es->st_shndx == SHN_UNDEF) {
				/* Undefined reference. */
				int idx = sym_find(name);
				if (idx < 0)
					idx = sym_add(name);
				o->sym_map[si] = idx;
				continue;
			}

			if (es->st_shndx == SHN_ABS) {
				int idx = sym_find(name);
				if (idx < 0)
					idx = sym_add(name);
				g.syms[idx].value = es->st_value;
				g.syms[idx].isec_idx = -1;
				g.syms[idx].defined = 1;
				g.syms[idx].binding = bind;
				o->sym_map[si] = idx;
				continue;
			}

			if (es->st_shndx == SHN_COMMON) {
				/* COMMON: allocate in BSS if not already defined. */
				int idx = sym_find(name);
				if (idx < 0)
					idx = sym_add(name);
				if (!g.syms[idx].defined) {
					/* Create a BSS input section for this COMMON symbol. */
					if (g.nisecs >= MAX_ISECS)
						die("too many sections");
					int isec = g.nisecs++;
					InputSection *is = &g.isecs[isec];
					is->obj = o;
					is->shndx = -1;
					is->kind = SECT_BSS;
					is->size = es->st_size;
					is->align = es->st_value;	/* COMMON: st_value = alignment. */
					if (is->align == 0)
						is->align = 1;
					is->data = NULL;

					g.syms[idx].isec_idx = isec;
					g.syms[idx].sec_offset = 0;
					g.syms[idx].defined = 1;
					g.syms[idx].binding = bind;
				}
				o->sym_map[si] = idx;
				continue;
			}

			/* Defined in a real section. */
			if (es->st_shndx >= o->ehdr->e_shnum)
				continue;
			int isec = o->sec_map[es->st_shndx];
			if (isec < 0)
				continue;	/* Discarded section. */

			int idx = sym_find(name);
			if (idx < 0)
				idx = sym_add(name);

			if (g.syms[idx].defined && bind == STB_GLOBAL
			    && g.syms[idx].binding == STB_GLOBAL) {
				fprintf(stderr,
					"ember-ld: duplicate symbol: %s\n", name);
				fprintf(stderr, "  defined in %s\n",
					g.objs[oi].path);
				die("link failed");
			}

			/* Define (or override WEAK with GLOBAL) */
			if (!g.syms[idx].defined || bind == STB_GLOBAL) {
				g.syms[idx].isec_idx = isec;
				g.syms[idx].sec_offset = es->st_value;
				g.syms[idx].defined = 1;
				g.syms[idx].binding = bind;
			}
			o->sym_map[si] = idx;
		}
	}
}

/* ---- Layout ---- */

static void
layout(void)
{
	/* Compute merged section sizes and assign offsets within each kind. */
	uint64_t kind_size[SECT_NKINDS] = { 0, 0, 0, 0 };

	for (int i = 0; i < g.nisecs; i++) {
		InputSection *is = &g.isecs[i];
		kind_size[is->kind] = align_up(kind_size[is->kind], is->align);
		is->out_offset = kind_size[is->kind];
		kind_size[is->kind] += is->size;
	}

	/* Assign VMAs: .text, .rodata, .got, .data, .bss -- each page-aligned. */
	uint64_t vma = g.vma_base;
	uint64_t file_off = 0x1000;	/* After ELF + program headers, page-aligned. */

	for (int k = 0; k < SECT_NKINDS; k++) {
		g.merged[k].vma = vma;
		g.merged[k].file_off = file_off;
		g.merged[k].size = kind_size[k];

		if (k == SECT_RODATA) {
			/* GOT goes right after rodata (no extra page break) */
			g.got_vma = vma + kind_size[k];
			g.got_file_off = file_off + kind_size[k];
			/* got_size set later by build_got(); advance vma/file_off after. */
		}

		uint64_t total = kind_size[k];
		if (k == SECT_RODATA)
			total += g.got_size;	/* Preliminary; updated after build_got. */
		if (total > 0 || k == SECT_TEXT) {
			vma = align_up(vma + total, 0x1000);
			if (k != SECT_BSS)
				file_off = align_up(file_off + total, 0x1000);
		}
	}

	/* Finalize symbol values. */
	for (int i = 0; i < g.nsyms; i++) {
		Symbol *s = &g.syms[i];
		if (s->isec_idx < 0)
			continue;	/* ABS or undefined. */
		InputSection *is = &g.isecs[s->isec_idx];
		s->value =
		    g.merged[is->kind].vma + is->out_offset + s->sec_offset;
	}
}

/* ---- GOT builder ---- */

static int
find_got_entry(int sym_idx)
{
	for (int i = 0; i < g.ngot; i++)
		if (g.got[i].sym_idx == sym_idx)
			return i;
	return -1;
}

static void
build_got(void)
{
	/* Scan all RELA sections for GOTPCREL, collect unique GOT entries. */
	g.ngot = 0;

	for (int oi = 0; oi < g.nobjs; oi++) {
		ObjFile *o = &g.objs[oi];
		for (int si = 0; si < o->ehdr->e_shnum; si++) {
			Elf64_Shdr *sh = &o->shdrs[si];
			if (sh->sh_type != SHT_RELA)
				continue;

			int nrels = (int)(sh->sh_size / sizeof(Elf64_Rela));
			Elf64_Rela *rels =
			    (Elf64_Rela *) (o->data + sh->sh_offset);

			for (int ri = 0; ri < nrels; ri++) {
				if (ELF64_R_TYPE(rels[ri].r_info) !=
				    R_X86_64_GOTPCREL)
					continue;

				uint32_t sym_idx = ELF64_R_SYM(rels[ri].r_info);
				if ((int)sym_idx >= o->nsyms)
					die("bad reloc sym index");
				int resolved = o->sym_map[sym_idx];
				if (resolved < 0)
					die("unresolved GOTPCREL symbol");

				if (find_got_entry(resolved) >= 0)
					continue;
				if (g.ngot >= MAX_GOT)
					die("too many GOT entries");
				g.got[g.ngot].sym_idx = resolved;
				g.got[g.ngot].got_offset =
				    (uint64_t) g.ngot * 8;
				g.ngot++;
			}
		}
	}

	g.got_size = (uint64_t) g.ngot * 8;
}

static void
relayout_with_got(void)
{
	/* Re-run layout now that GOT size is known. */
	uint64_t kind_size[SECT_NKINDS];
	for (int k = 0; k < SECT_NKINDS; k++)
		kind_size[k] = g.merged[k].size;

	uint64_t vma = g.vma_base;
	uint64_t file_off = 0x1000;

	for (int k = 0; k < SECT_NKINDS; k++) {
		g.merged[k].vma = vma;
		g.merged[k].file_off = file_off;

		uint64_t sec_size = kind_size[k];

		if (k == SECT_RODATA) {
			/* GOT immediately after rodata. */
			g.got_vma = vma + sec_size;
			g.got_file_off = file_off + sec_size;
			sec_size += g.got_size;
		}

		if (sec_size > 0 || k == SECT_TEXT) {
			vma = align_up(vma + sec_size, 0x1000);
			if (k != SECT_BSS)
				file_off =
				    align_up(file_off + sec_size, 0x1000);
		}
	}

	/* Re-finalize symbol values. */
	for (int i = 0; i < g.nsyms; i++) {
		Symbol *s = &g.syms[i];
		if (s->isec_idx < 0)
			continue;
		InputSection *is = &g.isecs[s->isec_idx];
		s->value =
		    g.merged[is->kind].vma + is->out_offset + s->sec_offset;
	}
}

/* ---- Relocation ---- */

static void
apply_relocations(uint8_t * buf)
{
	for (int oi = 0; oi < g.nobjs; oi++) {
		ObjFile *o = &g.objs[oi];
		for (int si = 0; si < o->ehdr->e_shnum; si++) {
			Elf64_Shdr *sh = &o->shdrs[si];
			if (sh->sh_type != SHT_RELA)
				continue;

			/* sh_info = index of section being relocated. */
			int target_shndx = (int)sh->sh_info;
			int target_isec = o->sec_map[target_shndx];
			if (target_isec < 0)
				continue;	/* Discarded section. */

			InputSection *is = &g.isecs[target_isec];
			uint64_t sec_vma =
			    g.merged[is->kind].vma + is->out_offset;
			uint64_t sec_foff =
			    g.merged[is->kind].file_off + is->out_offset;

			int nrels = (int)(sh->sh_size / sizeof(Elf64_Rela));
			Elf64_Rela *rels =
			    (Elf64_Rela *) (o->data + sh->sh_offset);

			for (int ri = 0; ri < nrels; ri++) {
				Elf64_Rela *r = &rels[ri];
				uint32_t rsym = ELF64_R_SYM(r->r_info);
				uint32_t rtype = ELF64_R_TYPE(r->r_info);

				if ((int)rsym >= o->nsyms)
					die("reloc: bad sym index");
				int sym_idx = o->sym_map[rsym];
				if (sym_idx < 0) {
					/* Symbol in discarded section -- skip silently. */
					continue;
				}

				Symbol *sym = &g.syms[sym_idx];
				if (!sym->defined) {
					fprintf(stderr,
						"ember-ld: undefined symbol: %s (in %s)\n",
						sym->name, o->path);
					die("link failed");
				}

				uint64_t S = sym->value;
				int64_t A = r->r_addend;
				uint64_t P = sec_vma + r->r_offset;
				uint8_t *patch = buf + sec_foff + r->r_offset;

				switch (rtype) {
				case R_X86_64_64:
					*(uint64_t *) patch = S + (uint64_t) A;
					break;

				case R_X86_64_PC32:
				case R_X86_64_PLT32:{
						int64_t val =
						    (int64_t) (S +
							       (uint64_t) A -
							       P);
						if (val < -0x80000000LL
						    || val > 0x7fffffffLL) {
							fprintf(stderr,
								"ember-ld: PC32 overflow for %s: %lld\n",
								sym->name,
								(long long)val);
							die("link failed");
						}
						*(int32_t *) patch =
						    (int32_t) val;
						break;
					}

				case R_X86_64_32:{
						uint64_t val = S + (uint64_t) A;
						if (val > 0xffffffffULL) {
							fprintf(stderr,
								"ember-ld: R_X86_64_32 overflow for %s\n",
								sym->name);
							die("link failed");
						}
						*(uint32_t *) patch =
						    (uint32_t) val;
						break;
					}

				case R_X86_64_32S:{
						int64_t val =
						    (int64_t) (S +
							       (uint64_t) A);
						if (val < -0x80000000LL
						    || val > 0x7fffffffLL) {
							fprintf(stderr,
								"ember-ld: 32S overflow for %s: %lld\n",
								sym->name,
								(long long)val);
							die("link failed");
						}
						*(int32_t *) patch =
						    (int32_t) val;
						break;
					}

				case R_X86_64_GOTPCREL:{
						int gi =
						    find_got_entry(sym_idx);
						if (gi < 0)
							die("GOTPCREL: missing GOT entry");
						uint64_t G =
						    g.got_vma +
						    g.got[gi].got_offset;
						int64_t val =
						    (int64_t) (G +
							       (uint64_t) A -
							       P);
						if (val < -0x80000000LL
						    || val > 0x7fffffffLL) {
							fprintf(stderr,
								"ember-ld: GOTPCREL overflow for %s\n",
								sym->name);
							die("link failed");
						}
						*(int32_t *) patch =
						    (int32_t) val;
						break;
					}

				default:
					fprintf(stderr,
						"ember-ld: unsupported reloc type %u for %s\n",
						rtype, sym->name);
					die("link failed");
				}
			}
		}
	}
}

/* ---- ELF64 output ---- */

static void
write_elf(const char *out_path, const char *entry_name)
{
	/* Find entry symbol. */
	int entry_idx = sym_find(entry_name);
	if (entry_idx < 0 || !g.syms[entry_idx].defined)
		dief("entry symbol not found: %s", entry_name);
	uint64_t entry_vma = g.syms[entry_idx].value;

	/*
	 * Compute total file size:
	 * 0x1000 for headers + padding
	 * then text + rodata + got + data (page-aligned between groups)
	 */
	uint64_t total_filesz =
	    g.merged[SECT_DATA].file_off + g.merged[SECT_DATA].size;
	if (g.merged[SECT_DATA].size == 0 && g.got_size > 0)
		total_filesz = g.got_file_off + g.got_size;
	if (g.merged[SECT_DATA].size == 0 && g.got_size == 0)
		total_filesz =
		    g.merged[SECT_RODATA].file_off + g.merged[SECT_RODATA].size;
	if (g.merged[SECT_RODATA].size == 0 && g.got_size == 0
	    && g.merged[SECT_DATA].size == 0)
		total_filesz =
		    g.merged[SECT_TEXT].file_off + g.merged[SECT_TEXT].size;

	/* Total memsz includes BSS. */
	uint64_t seg_vma_start = g.merged[SECT_TEXT].vma;
	uint64_t seg_vma_end = g.merged[SECT_BSS].vma + g.merged[SECT_BSS].size;
	if (g.merged[SECT_BSS].size == 0)
		seg_vma_end =
		    g.merged[SECT_DATA].vma + g.merged[SECT_DATA].size;
	uint64_t total_memsz = seg_vma_end - seg_vma_start;
	uint64_t total_file_data = total_filesz - 0x1000;	/* Segment data only. */

	/* Allocate output buffer (headers + segment data) */
	uint8_t *buf = (uint8_t *) calloc(1, (size_t) total_filesz);
	if (!buf)
		die("output buffer alloc failed");

	/* Copy section data to output buffer. */
	for (int i = 0; i < g.nisecs; i++) {
		InputSection *is = &g.isecs[i];
		if (!is->data)
			continue;	/* BSS. */
		uint64_t dst_off = g.merged[is->kind].file_off + is->out_offset;
		memcpy(buf + dst_off, is->data, (size_t) is->size);
	}

	/* Fill GOT entries. */
	for (int i = 0; i < g.ngot; i++) {
		Symbol *s = &g.syms[g.got[i].sym_idx];
		uint64_t addr = s->value;
		memcpy(buf + g.got_file_off + g.got[i].got_offset, &addr, 8);
	}

	/* Apply relocations. */
	apply_relocations(buf);

	/* Build ELF header. */
	Elf64_Ehdr *eh = (Elf64_Ehdr *) buf;
	eh->e_ident[0] = 0x7f;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_ident[4] = 2;	/* ELFCLASS64. */
	eh->e_ident[5] = 1;	/* ELFDATA2LSB. */
	eh->e_ident[6] = 1;	/* EV_CURRENT. */
	eh->e_type = ET_EXEC;
	eh->e_machine = EM_X86_64;
	eh->e_version = 1;
	eh->e_entry = entry_vma;
	eh->e_phoff = sizeof(Elf64_Ehdr);
	eh->e_phentsize = sizeof(Elf64_Phdr);
	eh->e_phnum = 1;
	eh->e_ehsize = sizeof(Elf64_Ehdr);
	/* No section headers. */
	eh->e_shoff = 0;
	eh->e_shnum = 0;
	eh->e_shentsize = sizeof(Elf64_Shdr);
	eh->e_shstrndx = 0;

	/* Build program header: single LOAD segment. */
	Elf64_Phdr *ph = (Elf64_Phdr *) (buf + sizeof(Elf64_Ehdr));
	ph->p_type = PT_LOAD;
	ph->p_flags = PF_R | PF_W | PF_X;
	ph->p_offset = 0x1000;
	ph->p_vaddr = seg_vma_start;
	ph->p_paddr = g.have_lma ? g.lma_base : seg_vma_start;
	ph->p_filesz = total_file_data;
	ph->p_memsz = total_memsz;
	ph->p_align = 0x1000;

	/* Write output. */
	FILE *fo = fopen(out_path, "wb");
	if (!fo)
		dief("cannot create: %s", out_path);
	if (fwrite(buf, 1, (size_t) total_filesz, fo) != (size_t) total_filesz)
		die("write failed");
	fclose(fo);

	free(buf);

	fprintf(stderr, "ember-ld: %s (entry=%s @ 0x%llx, %llu bytes)\n",
		out_path, entry_name,
		(unsigned long long)entry_vma,
		(unsigned long long)total_filesz);
}

/* ---- Linker-defined symbols ---- */

static void
add_linker_symbols(void)
{
	if (!g.have_lma)
		return;

	/* _Kernel_end = end of BSS, page-aligned. */
	uint64_t kernel_end =
	    align_up(g.merged[SECT_BSS].vma + g.merged[SECT_BSS].size, 0x1000);

	int idx = sym_find("_kernel_end");
	if (idx < 0)
		idx = sym_add("_kernel_end");
	g.syms[idx].value = kernel_end;
	g.syms[idx].isec_idx = -1;
	g.syms[idx].defined = 1;
	g.syms[idx].binding = STB_GLOBAL;

	/* _Kernel_size = _kernel_end - vma_base. */
	idx = sym_find("_kernel_size");
	if (idx < 0)
		idx = sym_add("_kernel_size");
	g.syms[idx].value = kernel_end - g.vma_base;
	g.syms[idx].isec_idx = -1;
	g.syms[idx].defined = 1;
	g.syms[idx].binding = STB_GLOBAL;
}

/* ---- Main ---- */

int
main(int argc, char **argv)
{
	const char *out_path = NULL;
	const char *entry = "_start";
	g.vma_base = 0;
	g.lma_base = 0;
	g.have_lma = 0;

	/* Parse arguments. */
	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		} else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
			entry = argv[++i];
		} else if (strncmp(argv[i], "--base=", 7) == 0) {
			g.vma_base = strtoull(argv[i] + 7, NULL, 0);
			g.lma_base = g.vma_base;
		} else if (strncmp(argv[i], "--vma=", 6) == 0) {
			g.vma_base = strtoull(argv[i] + 6, NULL, 0);
		} else if (strncmp(argv[i], "--lma=", 6) == 0) {
			g.lma_base = strtoull(argv[i] + 6, NULL, 0);
			g.have_lma = 1;
		} else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
			load_archive(argv[++i]);
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "ember-ld: unknown option: %s\n",
				argv[i]);
			return 1;
		} else {
			/* Positional argument: .o file. */
			load_obj(argv[i]);
		}
	}

	if (!out_path)
		die("no output file (-o)");
	if (g.vma_base == 0)
		die("no base address (--base or --vma)");
	if (g.nobjs == 0)
		die("no input files");

	/* Link pipeline. */
	resolve_symbols();
	layout();		/* Preliminary layout (GOT size unknown) */
	build_got();		/* Scan for GOTPCREL, determine GOT size. */
	relayout_with_got();	/* Re-layout with actual GOT size. */
	add_linker_symbols();
	write_elf(out_path, entry);

	return 0;
}
