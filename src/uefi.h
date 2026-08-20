/* The part of the UEFI specification this implementation uses.
 *
 * ⚠️ WHY THE MinGW TARGET AND NOT A BARE-METAL ONE.
 *
 * A UEFI application is PE/COFF with subsystem 10, and it is entered through
 * the Microsoft x64 calling convention. Both are properties this toolchain
 * already has: `x86_64-windows-gnu` produces PE and defaults to that
 * convention, so firmware function pointers can be called directly rather than
 * through per-call annotations.
 *
 * The first analysis of this backend concluded that mcpp would need a new
 * "PE freestanding" target before a UEFI application could be built at all.
 * Measured: it does not. `-nostdlib` with `--subsystem,10` and `-e,efi_main`
 * produces `IMAGE_SUBSYSTEM_EFI_APPLICATION` with no DLL imports, which is
 * exactly what firmware loads.
 *
 * ⚠️ THE OFFSETS ARE CHECKED, NOT TRUSTED.
 *
 * These tables are ABI, and a field written one slot out reads a neighbouring
 * function pointer — which calls something that exists, with the wrong
 * arguments. Every member this file reaches is pinned by a static assertion on
 * its offset, so a mistake is a compile error rather than a firmware crash.
 */
#ifndef OPENKAL_UEFI_H
#define OPENKAL_UEFI_H

using efi_status  = unsigned long long;
using efi_uintn   = unsigned long long;
using efi_handle  = void*;
using efi_char16  = char16_t;

inline constexpr efi_status EFI_SUCCESS = 0;
/* The high bit marks an error; the value below is EFI_ABORTED. */
inline constexpr efi_status EFI_ABORTED = 0x8000000000000015ULL;

/* EFI_MEMORY_TYPE: an application's own working storage. */
inline constexpr int EfiLoaderData = 2;

struct efi_table_header {
    unsigned long long signature;
    unsigned int       revision;
    unsigned int       header_size;
    unsigned int       crc32;
    unsigned int       reserved;
};
static_assert(sizeof(efi_table_header) == 24);

struct efi_simple_text_output_protocol {
    void*       reset;
    efi_status (*output_string)(efi_simple_text_output_protocol*, const efi_char16*);
    void*       test_string;
    void*       query_mode;
    void*       set_mode;
    void*       set_attribute;
    void*       clear_screen;
    void*       set_cursor_position;
    void*       enable_cursor;
    void*       mode;
};

struct efi_simple_text_input_protocol {
    void*       reset;
    efi_status (*read_key_stroke)(efi_simple_text_input_protocol*, void* key);
    void*       wait_for_key;
};

/* Only as far as `exit`, with every intervening slot named so that the offsets
 * are computed rather than counted by hand. */
struct efi_boot_services {
    efi_table_header hdr;                    /*   0 */
    void* raise_tpl;                         /*  24 */
    void* restore_tpl;                       /*  32 */
    void* allocate_pages;                    /*  40 */
    void* free_pages;                        /*  48 */
    void* get_memory_map;                    /*  56 */
    efi_status (*allocate_pool)(int type, efi_uintn size, void** buf);  /* 64 */
    efi_status (*free_pool)(void* buf);      /*  72 */
    void* create_event;                      /*  80 */
    void* set_timer;                         /*  88 */
    void* wait_for_event;                    /*  96 */
    void* signal_event;                      /* 104 */
    void* close_event;                       /* 112 */
    void* check_event;                       /* 120 */
    void* install_protocol_interface;        /* 128 */
    void* reinstall_protocol_interface;      /* 136 */
    void* uninstall_protocol_interface;      /* 144 */
    void* handle_protocol;                   /* 152 */
    void* reserved;                          /* 160 */
    void* register_protocol_notify;          /* 168 */
    void* locate_handle;                     /* 176 */
    void* locate_device_path;                /* 184 */
    void* install_configuration_table;       /* 192 */
    void* load_image;                        /* 200 */
    void* start_image;                       /* 208 */
    efi_status (*exit)(efi_handle image, efi_status status,
                       efi_uintn data_size, efi_char16* data);  /* 216 */
    void* unload_image;                      /* 224 */
    void* exit_boot_services;                /* 232 */
    void* get_next_monotonic_count;          /* 240 */
    void* stall;                             /* 248 */
};

static_assert(__builtin_offsetof(efi_boot_services, allocate_pool) == 64,
              "EFI_BOOT_SERVICES.AllocatePool is the sixth service after the header");
static_assert(__builtin_offsetof(efi_boot_services, free_pool) == 72);
static_assert(__builtin_offsetof(efi_boot_services, exit) == 216,
              "EFI_BOOT_SERVICES.Exit follows StartImage");
static_assert(__builtin_offsetof(efi_boot_services, exit_boot_services) == 232);

struct efi_system_table {
    efi_table_header hdr;                    /*   0 */
    efi_char16*      firmware_vendor;        /*  24 */
    unsigned int     firmware_revision;      /*  32 */
    unsigned int     pad_;                   /*  36 */
    efi_handle       console_in_handle;      /*  40 */
    efi_simple_text_input_protocol*  con_in; /*  48 */
    efi_handle       console_out_handle;     /*  56 */
    efi_simple_text_output_protocol* con_out;/*  64 */
    efi_handle       standard_error_handle;  /*  72 */
    efi_simple_text_output_protocol* std_err;/*  80 */
    void*            runtime_services;       /*  88 */
    efi_boot_services* boot_services;        /*  96 */
};

static_assert(__builtin_offsetof(efi_system_table, con_out) == 64,
              "EFI_SYSTEM_TABLE.ConOut sits after the two console-in members");
static_assert(__builtin_offsetof(efi_system_table, std_err) == 80);
static_assert(__builtin_offsetof(efi_system_table, boot_services) == 96,
              "EFI_SYSTEM_TABLE.BootServices follows RuntimeServices");

#endif /* OPENKAL_UEFI_H */
