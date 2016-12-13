/*
 * symdep.c
 *
 * This program checks symbol dependencies of prebuilt proprietary blobs
 * to compiled Android ROM ("out/target/product/..." directory)
 *
 * Author: Kostyan_nsk
 *
 * Copyright (C) 2016 Kostyan_nsk. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <elf.h>
#include <libgen.h>
#include <limits.h>
#include <bfd.h>

#define ARRAY_SIZE(x)	(sizeof(x)/sizeof(x[0]))

#define RED	"\x1b[1;31m"
#define GREEN	"\x1b[1;32m"
#define RESET	"\x1B[0m"

struct Elf_Ehdr {
    Elf32_Ehdr Ehdr32;
    Elf64_Ehdr Ehdr64;
};

struct Elf_Shdr {
    Elf32_Shdr Shdr32;
    Elf64_Shdr Shdr64;
};

struct Elf_Sym {
    Elf32_Sym Sym32;
    Elf64_Sym Sym64;
};

struct Elf_Dyn {
    Elf32_Dyn Dyn32;
    Elf64_Dyn Dyn64;
};

struct lib_list {
    uint16_t parent_id;
    unsigned char *name;
    struct lib_list *next;
};

struct sym_list {
    uint8_t found;
    uint16_t lib_id;
    unsigned char *symbol;
    struct sym_list *next;
};

struct shim_libs {
    unsigned char lib[NAME_MAX];
    unsigned char shim[NAME_MAX];
    uint8_t processed;
};

static uint8_t g_elf_class, g_cur_depth = 0, g_depth = 1, g_silent = 0, g_full = 0,
	g_path_cnt = 0, g_cust_path = 0, g_verbose = 0, g_shim_cnt = 0;
static struct sym_list *g_symlist = NULL;
static struct lib_list *g_liblist = NULL;
static struct shim_libs g_shimlibs[32];
static unsigned char g_padding[129];
static unsigned char g_paths[16][PATH_MAX];

static int read_header(int fd, struct Elf_Ehdr *elf_header) {
    int ret;
    size_t size;

    if (lseek(fd, 0, SEEK_SET) < 0)
	return -errno;

    if (g_elf_class == 32) {
	size = sizeof(Elf32_Ehdr);
	ret = read(fd, &elf_header->Ehdr32, size);
    }
    else {
	size = sizeof(Elf64_Ehdr);
	ret = read(fd, &elf_header->Ehdr64, size);
    }

    if (ret != size)
        return -EIO;

    return 0;
}

static struct Elf_Shdr* read_section_table(int fd, const struct Elf_Ehdr *elf_header) {

    size_t i, size;
    uint64_t offset;
    uint16_t num;
    int ret;
    struct Elf_Shdr *section_table;

    if (g_elf_class == 32) {
	offset = elf_header->Ehdr32.e_shoff;
	num = elf_header->Ehdr32.e_shnum;
    }
    else {
	offset = elf_header->Ehdr64.e_shoff;
	num = elf_header->Ehdr64.e_shnum;
    }

    section_table = (struct Elf_Shdr *)malloc(sizeof(struct Elf_Shdr) * num);
    if (section_table == NULL)
	return NULL;

    if (lseek(fd, offset, SEEK_SET) < 0)
	goto error;

    for (i = 0; i < num; i++) {
	if (g_elf_class == 32) {
	    size = sizeof(Elf32_Shdr);
	    ret = read(fd, &section_table[i].Shdr32, size);
	}
	else {
	    size = sizeof(Elf64_Shdr);
	    ret = read(fd, &section_table[i].Shdr64, size);
	}
	if (ret != size)
	    goto error;
    }

    return section_table;

error:
    free(section_table);
    return NULL;
}

static struct Elf_Dyn* read_dynamic_table(int fd, const struct Elf_Shdr *section) {

    size_t size, i, n;
    uint64_t offset;
    struct Elf_Dyn *dynamic_table;

    if (section == NULL)
	return NULL;

    if (g_elf_class == 32) {
	size = sizeof(Elf32_Dyn);
	n = section->Shdr32.sh_size / size;
	offset = section->Shdr32.sh_offset;
    }
    else {
	size = sizeof(Elf64_Dyn);
	n = section->Shdr64.sh_size / size;
	offset = section->Shdr64.sh_offset;
    }

    dynamic_table = (struct Elf_Dyn *)malloc(sizeof(struct Elf_Dyn) * n);
    if (dynamic_table == NULL)
	return NULL;

    if (lseek(fd, offset, SEEK_SET) < 0)
	goto error;

    for (i = 0; i < n; i++)
	if (g_elf_class == 32) {
	    if (read(fd, &dynamic_table[i].Dyn32, size) != size)
		goto error;
	}
	else {
	    if (read(fd, &dynamic_table[i].Dyn64, size) != size)
		goto error;
	}

    return dynamic_table;

error:
    free(dynamic_table);
    return NULL;
}

static struct Elf_Sym* read_symbol_table(int fd, const struct Elf_Shdr *section) {

    size_t size, i, n;
    uint64_t offset;
    struct Elf_Sym *symbol_table;

    if (section == NULL)
	return NULL;

    if (g_elf_class == 32) {
	size = sizeof(Elf32_Sym);
	n = section->Shdr32.sh_size / size;
	offset = section->Shdr32.sh_offset;
    }
    else {
	size = sizeof(Elf64_Sym);
	n = section->Shdr64.sh_size / size;
	offset = section->Shdr64.sh_offset;
    }

    symbol_table = (struct Elf_Sym *)malloc(sizeof(struct Elf_Sym) * n);
    if (symbol_table == NULL)
	return NULL;

    if (lseek(fd, offset, SEEK_SET) < 0)
	goto error;

    for (i = 0; i < n; i++)
	if (g_elf_class == 32) {
	    if (read(fd, &symbol_table[i].Sym32, size) != size)
		goto error;
	}
	else {
	    if (read(fd, &symbol_table[i].Sym64, size) != size)
		goto error;
	}

    return symbol_table;

error:
    free(symbol_table);
    return NULL;
}

static unsigned char* read_string_table(int fd, const struct Elf_Shdr *section) {

    size_t size;
    uint64_t offset;
    unsigned char *string_table;

    if (section == NULL)
        return NULL;

    if (g_elf_class == 32) {
	size = section->Shdr32.sh_size;
	offset = section->Shdr32.sh_offset;
    }
    else {
	size = section->Shdr64.sh_size;
	offset = section->Shdr64.sh_offset;
    }

    string_table = (unsigned char *)malloc(size);
    if (string_table == NULL)
	return NULL;

    if (lseek(fd, offset, SEEK_SET) < 0)
	goto error;

    if (read(fd, string_table, size) != size)
	goto error;

    return string_table;

error:
    free(string_table);
    return NULL;
}

static inline struct Elf_Shdr* section_by_type(const struct Elf_Ehdr *elf_header,
		    uint32_t section_type, struct Elf_Shdr *section_table)
{
    size_t i;
    uint16_t num;

    if (section_table == NULL)
	return NULL;

    if (g_elf_class == 32) {
	num = elf_header->Ehdr32.e_shnum;

	for (i = 0; i < num; i++) {
	    if (section_type == section_table[i].Shdr32.sh_type)
		return &section_table[i];
        }
    }
    else {
	num = elf_header->Ehdr64.e_shnum;

	for (i = 0; i < num; i++) {
	    if (section_type == section_table[i].Shdr64.sh_type)
		return &section_table[i];
        }
    }

    return NULL;
}

static inline struct Elf_Shdr* section_by_index(const struct Elf_Ehdr *elf_header,
			    uint32_t index, struct Elf_Shdr *section_table)
{
    uint16_t num;

    if (section_table == NULL)
	return NULL;

    if (g_elf_class == 32)
	num = elf_header->Ehdr32.e_shnum;
    else
	num = elf_header->Ehdr64.e_shnum;

    if (index >= num)
	return NULL;

    return &section_table[index];
}

static inline int add_in_lib_list(const unsigned char *libname, uint16_t parent_id) {

    struct lib_list *val, *last_val;
    uint16_t length, id = 0;

    /* Find the last value in list
     * and also check if lib is already in list
     */
    val = g_liblist;
    while (val != NULL) {
	if (!strcmp(val->name, libname))
	    return id;
	id++;
	last_val = val;
	val = val->next;
    }

    val = (struct lib_list *)malloc(sizeof(struct lib_list));
    if (val == NULL)
	return -1;

    length = strlen(libname);
    val->name = (unsigned char *)malloc(length + 1);
    if (val->name == NULL) {
	free(val);
	return -1;
    }
    memcpy(val->name, libname, length);
    val->name[length] = '\0';
    val->next = NULL;
    val->parent_id = parent_id;

    if (g_liblist == NULL)
	g_liblist = val;
    else
	last_val->next = val;

    return id;
}

static inline void add_in_sym_list(const unsigned char *symbol, uint16_t lib_id) {

    struct sym_list *val, *last_val;
    size_t length;

    /* Find the last value in list
     * and also check if symbol is already in list
     */
    val = g_symlist;
    while (val != NULL) {
	if (val->lib_id == lib_id && !strcmp(val->symbol, symbol))
	    return;
	last_val = val;
	val = val->next;
    }

    val = (struct sym_list *)malloc(sizeof(struct sym_list));
    if (val == NULL)
	return;

    length = strlen(symbol);
    val->symbol = (unsigned char *)malloc(length + 1);
    if (val->symbol == NULL) {
	free(val);
	return;
    }
    memcpy(val->symbol, symbol, length);
    val->symbol[length] = '\0';
    val->found = 0;
    val->lib_id = lib_id;
    val->next = NULL;

    if (g_symlist == NULL)
	g_symlist = val;
    else
	last_val->next = val;
}

static int open_lib(const unsigned char *libname) {

    size_t i;
    unsigned char *path, full_path[PATH_MAX];

    for (i = 0; i < g_path_cnt; i++) {
	path = basename(g_paths[i]);
	/* Look for lib in provided custom directories
	 * and appropriate to our ELF class directories
	 */
	if (i < g_cust_path || (!strcmp(path, "lib") && g_elf_class == 32)
	    || (!strcmp(path, "lib64") && g_elf_class == 64))
	{
		sprintf(full_path, "%s/%s", g_paths[i], libname);
		if (!access(full_path, R_OK))
		    return open(full_path, O_RDONLY);
	}
    }

    errno = ENOENT;
    return -1;
}

static inline int has_shim(const unsigned char *libname) {

    size_t i;

    for (i = 0; i < g_shim_cnt; i++)
	if (!strcmp(libname, g_shimlibs[i].lib))
	    return i;

    return -1;
}

static int process_lib(const unsigned char *libname, uint16_t id, uint16_t parent_id) {

    int i, n, fd, ret = 0;
    uint8_t ident[EI_NIDENT];
    struct Elf_Ehdr elf_header;
    struct Elf_Shdr *dynamic, *dynsym, *dynstr, *section_table;
    struct Elf_Sym *symbol_table;
    struct Elf_Dyn *dynamic_table;
    struct sym_list *sym_val;
    unsigned char *string_table;



    /* Padding */
    memset(g_padding, ' ', (g_cur_depth) * 4);
    g_padding[(g_cur_depth) * 4] = '\0';

    g_cur_depth++;

    /* At the first pass, open lib explicitly.
     * Otherwise, look for lib in directories
     */
    if (id == 0)
	fd = open(libname, O_RDONLY);
    else
	fd = open_lib(libname);

    if (fd < 0) {
	ret = errno;
	printf("%s%s: " RED "%s" RESET "\n", g_padding, libname, strerror(ret));
	goto exit;
    }

    if (read(fd, ident, EI_NIDENT) != EI_NIDENT) {
	ret = errno;
	printf("%s%s: " RED "%s" RESET "\n", g_padding, libname, strerror(ret));
	goto exit_file;
    }

    if (strncmp(ident, ELFMAG, SELFMAG) != 0) {
	printf("%s%s: " RED "Not ELF format" RESET "\n", g_padding, libname);
	ret = EILSEQ;
	goto exit_file;
    }

    if (id == 0)
	switch (ident[EI_CLASS]) {
	    case ELFCLASS32:
		g_elf_class = 32;
		break;
	    case ELFCLASS64:
		g_elf_class = 64;
		break;
	    case ELFCLASSNONE:
	    default:
		printf("%s%s: " RED "Invalid ELF class" RESET "\n", g_padding, libname);
		ret = EINVAL;
		goto exit_file;
	}
    else
	if (ident[EI_CLASS] != g_elf_class) {
	    if (g_elf_class == 32)
		printf("%s%s: " RED "Not ELF32 class" RESET "\n", g_padding, libname);
	    else
		printf("%s%s: " RED "Not ELF64 class" RESET "\n", g_padding, libname);

	    ret = EINVAL;
	    goto exit_file;
	}

    if (ident[EI_DATA] == ELFDATANONE || ident[EI_DATA] == ELFDATA2MSB) {
	printf("%s%s: " RED "not little endian data" RESET "\n", g_padding, libname);
	ret = EINVAL;
	goto exit_file;
    }


    ret = read_header(fd, &elf_header);
    if (ret < 0) {
	printf("%s%s: " RED "Error occured while reading ELF header: %s" RESET "\n", g_padding, libname, strerror(-ret));
	goto exit_file;
    }

    section_table = read_section_table(fd, &elf_header);
    if (section_table == NULL) {
	printf("%s%s: " RED "Error occured while reading section table" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_file;
    }

    dynamic = section_by_type(&elf_header, SHT_DYNAMIC, section_table);
    if (dynamic == NULL) {
	printf("%s%s: " RED "Error occured while reading .dynamic section header" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_section;
    }

    dynamic_table = read_dynamic_table(fd, dynamic);
    if (dynamic_table == NULL) {
	printf("%s%s " RED "Error occured while reading table for section .dynamic" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_section;
    }

    dynsym = section_by_type(&elf_header, SHT_DYNSYM, section_table);
    if (dynsym == NULL) {
	printf("%s%s: " RED "Error occured while reading .dynsym section header" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_dynamic;
    }

    symbol_table = read_symbol_table(fd, dynsym);
    if (symbol_table == NULL) {
	printf("%s%s: " RED "Error occured while reading table for section .dynsym" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_dynamic;
    }

    if (g_elf_class == 32)
	dynstr = section_by_index(&elf_header, dynsym->Shdr32.sh_link, section_table);
    else
	dynstr = section_by_index(&elf_header, dynsym->Shdr64.sh_link, section_table);
    if (dynstr == NULL) {
	printf("%s%s: " RED "Error occured while reading table for section .dynsym" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_dynamic;
    }

    string_table = read_string_table(fd, dynstr);
    if (string_table == NULL) {
	printf("%s%s: " RED "Error occured while reading table for section .strtab" RESET "\n", g_padding, libname);
	ret = EFAULT;
	goto exit_symbol;
    }

    if (!g_silent)
	printf("%s%s\n", g_padding, libname);

    /* Fill in list of required symbols */
    if (g_cur_depth <= g_depth || g_full) {
	if (g_elf_class == 32) {
	    n = dynsym->Shdr32.sh_size / sizeof(Elf32_Sym);
	    for (i = 0; i < n; i++) {
		if (symbol_table[i].Sym32.st_shndx == SHN_UNDEF
		    /* Skip weak symbols */
		    && ELF32_ST_BIND(symbol_table[i].Sym32.st_info) != STB_WEAK)
		{
			if (strlen(&string_table[symbol_table[i].Sym32.st_name]) > 0)
			    add_in_sym_list(&string_table[symbol_table[i].Sym32.st_name], id);
		}
	    }
	}
	else {
	    n = dynsym->Shdr64.sh_size / sizeof(Elf64_Sym);
	    for (i = 0; i < n; i++) {
		if (symbol_table[i].Sym64.st_shndx == SHN_UNDEF
		    /* Skip weak symbols */
		    && ELF64_ST_BIND(symbol_table[i].Sym64.st_info) != STB_WEAK)
		{
			if (strlen(&string_table[symbol_table[i].Sym64.st_name]) > 0)
			    add_in_sym_list(&string_table[symbol_table[i].Sym64.st_name], id);
		}
	    }
	}
    }

    /* Look for required symbols */
    if (id != 0) {
	if (g_elf_class == 32) {
	    n = dynsym->Shdr32.sh_size / sizeof(Elf32_Sym);
	    for (i = 0; i < n; i++) {
		if (symbol_table[i].Sym32.st_shndx != SHN_UNDEF) {
		    struct Elf_Shdr *section = section_by_index(&elf_header, symbol_table[i].Sym32.st_shndx, section_table);
		    /* Symbol is in .data or .bss section */
		    if (section != NULL && (section->Shdr32.sh_type == SHT_PROGBITS || section->Shdr32.sh_type == SHT_NOBITS)) {
			struct sym_list *sym_val = g_symlist;
			while (sym_val != NULL) {
			    if (sym_val->lib_id == parent_id) {
				if( !strcmp(&string_table[symbol_table[i].Sym32.st_name], sym_val->symbol)) {
				    sym_val->found = 1;
				    /* Print out found symbol if -v arg was supplied */
				    if (g_verbose)
					printf("%s%s -> %s\n", g_padding, libname, sym_val->symbol);
				    break;
				}
			    }
			    sym_val = sym_val->next;
			}
		    }
		}
	    }
	}
	else {
	    n = dynsym->Shdr64.sh_size / sizeof(Elf64_Sym);
	    for (i = 0; i < n; i++) {
		if (symbol_table[i].Sym64.st_shndx != SHN_UNDEF) {
		    struct Elf_Shdr *section = section_by_index(&elf_header, symbol_table[i].Sym64.st_shndx, section_table);
		    /* Symbol is in .data or .bss section */
		    if (section != NULL && (section->Shdr64.sh_type == SHT_PROGBITS || section->Shdr64.sh_type == SHT_NOBITS)) {
			struct sym_list *sym_val = g_symlist;
			while (sym_val != NULL) {
			    if (sym_val->lib_id == parent_id) {
				if( !strcmp(&string_table[symbol_table[i].Sym64.st_name], sym_val->symbol)) {
				    sym_val->found = 1;
				    /* Print out found symbol if -v arg was supplied */
				    if (g_verbose)
					printf("%s%s -> %s\n", g_padding, libname, sym_val->symbol);
				    break;
				}
			    }
			    sym_val = sym_val->next;
			}
		    }
		}
	    }
	}
    }

    /* Process shim lib */
    if ((i = has_shim(libname)) >= 0 && !g_shimlibs[i].processed)
	if ((n = add_in_lib_list(g_shimlibs[i].shim, parent_id)) > 0) {
	    g_cur_depth--;
	    /* Avoid dead loop when shim lib
	    * depends from its counterpart
	    */
	    g_shimlibs[i].processed = 1;

	    process_lib(g_shimlibs[i].shim, n, parent_id);
	    g_cur_depth++;
	}

    /* Read DT_NEEDED from .dynamic section table
     * and process required libs
     */
    if (g_cur_depth <= g_depth || g_full) {
	if (g_elf_class == 32) {
	    n = dynamic->Shdr32.sh_size / sizeof(Elf32_Dyn);
	    for (i = 0; i < n; i++) {
		if (dynamic_table[i].Dyn32.d_tag == DT_NULL)
		    break;
		if (dynamic_table[i].Dyn32.d_tag == DT_NEEDED) {
		    int new_id;
		    if (new_id = add_in_lib_list(&string_table[dynamic_table[i].Dyn32.d_un.d_ptr], id) > 0)
			ret = process_lib(&string_table[dynamic_table[i].Dyn32.d_un.d_ptr], new_id, id);
		}
	    }
	}
	else {
	    n = dynamic->Shdr64.sh_size / sizeof(Elf64_Dyn);
	    for (i = 0; i < n; i++) {
		if (dynamic_table[i].Dyn64.d_tag == DT_NULL)
		    break;
		if (dynamic_table[i].Dyn64.d_tag == DT_NEEDED) {
		    int new_id;
		    if (new_id = add_in_lib_list(&string_table[dynamic_table[i].Dyn64.d_un.d_ptr], id) > 0)
			ret = process_lib(&string_table[dynamic_table[i].Dyn64.d_un.d_ptr], new_id, id);
		}
	    }
	}
    }

    free(string_table);
exit_symbol:
    free(symbol_table);
exit_dynamic:
    free(dynamic_table);
exit_section:
    free(section_table);
exit_file:
    close(fd);
exit:
    g_cur_depth--;
    return ret;
}

static void add_dir(const unsigned char *parent_path, const unsigned char *dir) {

    sprintf(g_paths[g_path_cnt], "%s%s", parent_path, dir);
    if (!access(g_paths[g_path_cnt], F_OK))
	g_path_cnt++;
}

static int strpos(const char *str, const char *substr) {

    char *ret = strstr(str, substr);
    if (ret == NULL)
	return -1;
    else
	return ret - str;
}

static unsigned char* str_replace(unsigned char *str, const unsigned char *substr,
						const unsigned char *replace)
{
    static unsigned char buf[PATH_MAX];
    size_t len;
    int pos;

    pos = strpos(str, substr);
    if (pos < 0 || (strlen(str) - strlen(substr) + strlen(replace)) >= PATH_MAX)
	return str;

    len = strlen(substr);
    strncpy(buf, str, pos);
    sprintf(buf + pos, "%s%s", replace, str + pos + len);
    return buf;
}

static unsigned char* get_lib_by_id(uint16_t lib_id) {

    uint16_t k = 0;
    struct lib_list *lib_val = g_liblist;

    while (lib_val != NULL) {
	if (k == lib_id)
	    return lib_val->name;
	lib_val = lib_val->next;
	k++;
    }

    return NULL;
}

static void usage(char * program_name) {

    printf("Usage: %s [option(s)] <file>\n", program_name);
    printf(" Lists external symbols of prebuilt proprietary ELF <file> which\n"
	" were not found in needed compiled Android's shared objects.\n"
	" <file> assumed to be in out/target/product//system/bin/ or\n"
	"			 out/target/product//system/lib*/ or\n"
	"			 out/target/product//system/vendor/lib*/\n");
    printf(" The options are:\n");
    printf(" -v, --verbose		Show found symbols\n");
    printf(" -s, --silent		Show result only\n");
    printf(" --depth <n>		Set recursion depth to <n>, default value is 1\n");
    printf(" --full			Full depth recursion\n");
    printf(" -i <path>		Include custom paths where to look for needed shared objects\n");
    printf("			Use colon-separated list in case of multiple values\n");
    printf(" --shim <lib|shim>	Supply shim counterpart for shared object\n");
    printf("			Use colon-separated list in case of multiple values\n");
    printf(" --demangle		Decode low-level symbol names into user-level names\n");
    printf(" -h, --help		Display this information\n\n");
    printf("Report bugs to: https://github.com/Kostyan-nsk/symdep/issues\n");
}

int main(int argc, char **argv) {

    int i, id, ret;
    uint8_t g_demangle = 0, all_found = 1;
    unsigned char *home, *full_path, name[NAME_MAX], parent_path[PATH_MAX];
    struct sym_list *sym_val;

    if (argc < 2) {
	usage(*argv);
	return EINVAL;
    }

    home = getenv("HOME");

    /* Parsing arguments */
    for (i = 1; i < argc; i++) {
	/* Verbose */
	if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
	    g_verbose = 1;

	/* Silent */
	if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--silent"))
	    g_silent = 1;

	/* Help */
	if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
	    usage(*argv);
	    return 0;
	}

	/* Full depth recursion */
	if (!strcmp(argv[i], "--full"))
	    g_full = 1;

	/* Resolve symbol names */
	if (!strcmp(argv[i], "--demangle"))
	    g_demangle = 1;

	/* Recursion depth */
	if (!strcmp(argv[i], "--depth")) {
	    if (i + 1 == argc) {
		printf("Missing value for argument \"--depth\"\n");
		return EINVAL;
	    }
	    else {
		char *pEnd;
		g_depth = strtol(argv[i + 1], &pEnd, 0);
		if (g_depth <= 0 || errno == ERANGE) {
		    printf("Invalid value for argument \"--depth\"\n");
		    return EINVAL;
		}
		i++;
	    }
	}

	/* Custom directories */
	if (!strcmp(argv[i], "-i")) {
	    if (i + 1 == argc) {
		printf("Missing value for argument \"-i\"\n");
		return EINVAL;
	    }
	    else {
		char *p = strtok(argv[i + 1], ":");
		while (p != NULL) {
		    if ((full_path = realpath(str_replace(p, "~", home), NULL)) == NULL)
			printf("Warning: \"%s\": %s\n", p, strerror(errno));
		    else {
			add_dir(full_path, "");
			free(full_path);
			g_cust_path++;
		    }
		    p = strtok(NULL, ":");
		}
		i++;
	    }
	}

	/* Shim libs */
	if (!strcmp(argv[i], "--shim"))
	    if (i + 1 == argc) {
		printf("Missing value for argument \"-s\"\n");
		return EINVAL;
	    }
	else {
	    char *p = strtok(argv[i + 1], ":");
	    if (p != NULL) {
		while (p != NULL) {
		    if (strstr(p, "|") != NULL) {
			strncpy(g_shimlibs[g_shim_cnt].lib, p, strpos(p, "|"));
			strcpy(g_shimlibs[g_shim_cnt].shim, strpbrk(p, "|") + 1);
			g_shimlibs[g_shim_cnt].processed = 0;
			g_shim_cnt++;
		    }
		    else
			printf("Warning: Invalid value for argument \"--shim\": %s\n", p);
		    p = strtok(NULL, ":");
		}
	    }
	    else {
		printf("Invalid value for argument \"--shim\"\n");
		return EINVAL;
	    }
	    i++;
	}
    }

    /* Silent overrides Verbose */
    if (g_silent)
	g_verbose = 0;

    /* Assume the last parameter is target lib name */
    full_path = realpath(str_replace(argv[argc - 1], "~", home), NULL);
    if (access(full_path, R_OK) < 0) {
	printf("%s: " RED "%s" RESET "\n", argv[argc - 1], strerror(errno));
	return errno;
    }

    /* Parsing paths */
    strcpy(name, basename(dirname(full_path)));
    strcpy(parent_path, dirname(full_path));
    free(full_path);

    /* Assume target ELF object is in
     *     system/vendor/bin
     *     system/vendor/sbin
     *     system/vendor/xbin
     *     system/vendor/lib
     *     system/vendor/lib64
     *     system/vendor/lib/hw
     *     system/vendor/lib64/hw
     *     system/lib
     *     system/lib64
     *     system/lib/hw
     *     system/lib64/hw
     * directories
     */
    // In case if we are in "lib*/hw" directory
    if((!strcmp(basename(parent_path), "lib") || !strcmp(basename(parent_path), "lib64"))
	&& !strcmp(name, "hw"))
	    strcpy(parent_path, dirname(parent_path));
    if (!strcmp(name, "lib") || !strcmp(name, "lib64") || !strcmp(name, "hw")
	|| !strcmp(name, "bin") || !strcmp(name, "sbin") || !strcmp(name, "xbin")) {

	    /* In case if we are in system/vendor/lib* directory */
	    if( !strcmp(basename(parent_path), "vendor"))
		strcpy(parent_path, dirname(parent_path));

	    if( !strcmp(basename(parent_path), "system")) {
		add_dir(parent_path, "/vendor/lib");
		add_dir(parent_path, "/vendor/lib64");
		add_dir(parent_path, "/lib");
		add_dir(parent_path, "/lib64");
	    }
    }

    strcpy(name, argv[argc - 1]);
    id = add_in_lib_list(basename(name), 0);
    if (id < 0)
	return errno;

    full_path = realpath(str_replace(argv[argc - 1], "~", home), NULL);
    /* And here we go in */
    ret = process_lib(full_path, id, 0);
    free(full_path);

    /* Check if all symbols were found */
    sym_val = g_symlist;
    while (sym_val != NULL) {
	if (!sym_val->found) {
	    all_found = 0;
	    break;
	}
	sym_val = sym_val->next;
    }

    if (all_found) {
	printf("\n" GREEN "All symbols found!" RESET "\n");
	return ret;
    }

    printf("\nCannot locate symbols:\n");
    sym_val = g_symlist;
    while (sym_val != NULL) {
	if (!sym_val->found) {
	    unsigned char *libname = get_lib_by_id(sym_val->lib_id);
	    if (g_depth > 1 || g_full)
		printf("%s -> " RED "%s" RESET "\n", libname, sym_val->symbol);
	    else
		printf(RED "%s" RESET "\n", sym_val->symbol);

	    if (g_demangle) {
		unsigned char *demangled = bfd_demangle(0, sym_val->symbol, 0x101);
		if (demangled != NULL) {
		    if (g_depth > 1 || g_full) {
			memset(g_padding, ' ', strlen(libname) + 4);
			g_padding[strlen(libname) + 4] = '\0';
			printf("%s%s\n", g_padding, demangled);
		    }
		    else
			printf("%s\n", demangled);
		}
	    }
	}
	sym_val = sym_val->next;
    }

    return ret;
}
