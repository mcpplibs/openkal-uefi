/* The part of the UEFI specification this implementation uses.
 *
 * WHY THE MinGW TARGET AND NOT A BARE-METAL ONE.
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
 * THE OFFSETS ARE CHECKED, NOT TRUSTED.
 *
 * These tables are ABI, and a field written one slot out reads a neighbouring
 * function pointer — which calls something that exists, with the wrong
 * arguments. Every member this file reaches is pinned by a static assertion on
 * its offset, so a mistake is a compile error rather than a firmware crash.
 */
#ifndef OPENKAL_UEFI_H
#define OPENKAL_UEFI_H

using efi_status  = unsigned long long;
using efi_u8      = unsigned char;
using efi_u16     = unsigned short;
using efi_u32     = unsigned int;
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

/* One entry of the memory map (EFI_MEMORY_DESCRIPTOR). Entries are `descriptor_size' bytes apart,
 * which may exceed this structure; a reader steps by that size and never by sizeof. */
struct efi_memory_descriptor {
    unsigned int       type;
    unsigned int       pad;
    unsigned long long physical_start;
    unsigned long long virtual_start;
    unsigned long long pages;
    unsigned long long attribute;
};
inline constexpr unsigned int EfiConventionalMemory = 7;
inline constexpr efi_status EFI_BUFFER_TOO_SMALL = 0x8000000000000005ULL;

/* Only as far as `exit`, with every intervening slot named so that the offsets
 * are computed rather than counted by hand. */
struct efi_boot_services {
    efi_table_header hdr;                    /*   0 */
    void* raise_tpl;                         /*  24 */
    void* restore_tpl;                       /*  32 */
    efi_status (*allocate_pages)(int type, int memory_type, efi_uintn pages,
                                 unsigned long long* memory);          /* 40 */
    efi_status (*free_pages)(unsigned long long memory, efi_uintn pages);  /* 48 */
    efi_status (*get_memory_map)(efi_uintn* size, struct efi_memory_descriptor* map, efi_uintn* key,
                                 efi_uintn* descriptor_size, unsigned int* version);  /* 56 */
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
    efi_status (*handle_protocol)(efi_handle, const struct efi_guid*, void**);  /* 152 */
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
    efi_status (*stall)(efi_uintn microseconds);                       /* 248 */
    efi_status (*set_watchdog_timer)(efi_uintn seconds, unsigned long long code,
                                     efi_uintn data_size, efi_char16* data);  /* 256 */
    void* connect_controller;                /* 264 */
    void* disconnect_controller;             /* 272 */
    void* open_protocol;                     /* 280 */
    void* close_protocol;                    /* 288 */
    void* open_protocol_information;         /* 296 */
    void* protocols_per_handle;              /* 304 */
    void* locate_handle_buffer;              /* 312 */
    efi_status (*locate_protocol)(const struct efi_guid*, void* registration, void** iface);  /* 320 */
};

static_assert(__builtin_offsetof(efi_boot_services, allocate_pool) == 64,
              "EFI_BOOT_SERVICES.AllocatePool is the sixth service after the header");
static_assert(__builtin_offsetof(efi_boot_services, free_pool) == 72);
static_assert(__builtin_offsetof(efi_boot_services, exit) == 216,
              "EFI_BOOT_SERVICES.Exit follows StartImage");
static_assert(__builtin_offsetof(efi_boot_services, exit_boot_services) == 232);
static_assert(__builtin_offsetof(efi_boot_services, handle_protocol) == 152);
static_assert(__builtin_offsetof(efi_boot_services, stall) == 248);
static_assert(__builtin_offsetof(efi_boot_services, set_watchdog_timer) == 256);
static_assert(__builtin_offsetof(efi_boot_services, allocate_pages) == 40);
static_assert(__builtin_offsetof(efi_boot_services, free_pages) == 48);
static_assert(__builtin_offsetof(efi_boot_services, get_memory_map) == 56);
static_assert(__builtin_offsetof(efi_boot_services, locate_protocol) == 320);

/* EFI_ALLOCATE_TYPE and EFI_MEMORY_TYPE: the two this implementation asks for. */
enum { AllocateAnyPages = 0, AllocateAddress = 2 };
enum { EfiLoaderCode = 1 };

struct efi_guid { efi_u32 a; efi_u16 b, c; efi_u8 d[8]; };

/* EFI_TIME: the wall clock as the firmware keeps it, in its own time zone. */
struct efi_time {
    efi_u16 year;
    efi_u8  month, day, hour, minute, second, pad1;
    efi_u32 nanosecond;
    short   time_zone;           /* minutes from UTC; 2047: unspecified (local) */
    efi_u8  daylight, pad2;
};
static_assert(sizeof(efi_time) == 16);

/* As far as GetTime, the first service after the header. */
struct efi_runtime_services {
    efi_table_header hdr;                    /*   0 */
    efi_status (*get_time)(efi_time*, void* capabilities);  /* 24 */
};
static_assert(__builtin_offsetof(efi_runtime_services, get_time) == 24);

/* EFI_LOADED_IMAGE_PROTOCOL: what the firmware recorded about this image, and
 * among it the options it was started with (UCS-2 text when a shell or a boot
 * option gives a command line) and the device it was loaded from (the volume
 * openkal.fs supplies). */
struct efi_loaded_image_protocol {
    efi_u32     revision;
    efi_handle  parent_handle;
    void*       system_table;
    efi_handle  device_handle;
    void*       file_path;
    void*       reserved;
    efi_u32     load_options_size;
    void*       load_options;
    void*       image_base;
    unsigned long long image_size;
};
static_assert(__builtin_offsetof(efi_loaded_image_protocol, device_handle) == 24);
static_assert(__builtin_offsetof(efi_loaded_image_protocol, load_options_size) == 48);
static_assert(__builtin_offsetof(efi_loaded_image_protocol, load_options) == 56);

inline constexpr efi_guid kLoadedImageProtocol = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

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
    efi_runtime_services* runtime_services;  /*  88 */
    efi_boot_services* boot_services;        /*  96 */
};

static_assert(__builtin_offsetof(efi_system_table, con_out) == 64,
              "EFI_SYSTEM_TABLE.ConOut sits after the two console-in members");
static_assert(__builtin_offsetof(efi_system_table, std_err) == 80);
static_assert(__builtin_offsetof(efi_system_table, boot_services) == 96,
              "EFI_SYSTEM_TABLE.BootServices follows RuntimeServices");

/* EFI_MEMORY_ATTRIBUTE_PROTOCOL (UEFI 2.10): the protection of a range of memory. Firmware that
 * enforces "no page both writable and executable" offers it; older firmware maps every page both,
 * and has nothing to ask. */
struct efi_memory_attribute_protocol {
    efi_status (*get_memory_attributes)(efi_memory_attribute_protocol*, unsigned long long base,
                                        unsigned long long length, unsigned long long* attributes);
    efi_status (*set_memory_attributes)(efi_memory_attribute_protocol*, unsigned long long base,
                                        unsigned long long length, unsigned long long attributes);
    efi_status (*clear_memory_attributes)(efi_memory_attribute_protocol*, unsigned long long base,
                                          unsigned long long length, unsigned long long attributes);
};
inline constexpr efi_guid kMemoryAttributeProtocol = {
    0xf4560cf6, 0x40ec, 0x4b4a, {0xa1, 0x92, 0xbf, 0x1d, 0x57, 0xd0, 0xb1, 0x89}};
inline constexpr unsigned long long EFI_MEMORY_XP = 0x0000000000004000ull;
inline constexpr unsigned long long EFI_MEMORY_RO = 0x0000000000020000ull;

/* EFI status codes this implementation distinguishes (UEFI 2.10, appendix D). The high bit marks an
 * error; a warning has it clear and is not a failure. */
inline constexpr efi_status EFI_INVALID_PARAMETER = 0x8000000000000002ULL;
inline constexpr efi_status EFI_UNSUPPORTED       = 0x8000000000000003ULL;
inline constexpr efi_status EFI_BAD_BUFFER_SIZE   = 0x8000000000000004ULL;
inline constexpr efi_status EFI_NOT_READY         = 0x8000000000000006ULL;
inline constexpr efi_status EFI_DEVICE_ERROR      = 0x8000000000000007ULL;
inline constexpr efi_status EFI_WRITE_PROTECTED   = 0x8000000000000008ULL;
inline constexpr efi_status EFI_OUT_OF_RESOURCES  = 0x8000000000000009ULL;
inline constexpr efi_status EFI_VOLUME_CORRUPTED  = 0x800000000000000AULL;
inline constexpr efi_status EFI_VOLUME_FULL       = 0x800000000000000BULL;
inline constexpr efi_status EFI_NO_MEDIA          = 0x800000000000000CULL;
inline constexpr efi_status EFI_MEDIA_CHANGED     = 0x800000000000000DULL;
inline constexpr efi_status EFI_NOT_FOUND         = 0x800000000000000EULL;
inline constexpr efi_status EFI_ACCESS_DENIED     = 0x800000000000000FULL;
/* Delete closed the handle and did not delete the file (EFI_FILE_PROTOCOL.Delete). */
inline constexpr efi_status EFI_WARN_DELETE_FAILURE = 2;
inline constexpr bool efi_error(efi_status s) { return (s >> 63) != 0; }

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL (UEFI 2.10, 13.4): a volume, opened once for its root directory. */
struct efi_file_protocol;
struct efi_simple_file_system_protocol {
    unsigned long long revision;                                                              /* 0 */
    efi_status (*open_volume)(efi_simple_file_system_protocol*, efi_file_protocol** root);   /* 8 */
};
static_assert(__builtin_offsetof(efi_simple_file_system_protocol, open_volume) == 8);
inline constexpr efi_guid kSimpleFileSystemProtocol = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* EFI_FILE_PROTOCOL (UEFI 2.10, 13.5): a file or a directory open on a volume, each operation a
 * member of the handle itself. As far as Flush; the revision-2 members (OpenEx and those after it)
 * are asynchronous forms this implementation does not use. */
struct efi_file_protocol {
    unsigned long long revision;                                                                 /*  0 */
    efi_status (*open)(efi_file_protocol*, efi_file_protocol** out, const efi_char16* name,
                       unsigned long long mode, unsigned long long attributes);                  /*  8 */
    efi_status (*close)(efi_file_protocol*);                                                     /* 16 */
    efi_status (*remove)(efi_file_protocol*);       /* Delete: closes the handle in every case       24 */
    efi_status (*read)(efi_file_protocol*, efi_uintn* size, void* buffer);                       /* 32 */
    efi_status (*write)(efi_file_protocol*, efi_uintn* size, const void* buffer);                /* 40 */
    efi_status (*get_position)(efi_file_protocol*, unsigned long long* position);                /* 48 */
    efi_status (*set_position)(efi_file_protocol*, unsigned long long position);                 /* 56 */
    efi_status (*get_info)(efi_file_protocol*, const efi_guid* type, efi_uintn* size, void* buffer);  /* 64 */
    efi_status (*set_info)(efi_file_protocol*, const efi_guid* type, efi_uintn size, const void* buffer);  /* 72 */
    efi_status (*flush)(efi_file_protocol*);                                                     /* 80 */
};
static_assert(__builtin_offsetof(efi_file_protocol, open) == 8);
static_assert(__builtin_offsetof(efi_file_protocol, close) == 16);
static_assert(__builtin_offsetof(efi_file_protocol, remove) == 24);
static_assert(__builtin_offsetof(efi_file_protocol, read) == 32);
static_assert(__builtin_offsetof(efi_file_protocol, write) == 40);
static_assert(__builtin_offsetof(efi_file_protocol, get_position) == 48);
static_assert(__builtin_offsetof(efi_file_protocol, set_position) == 56);
static_assert(__builtin_offsetof(efi_file_protocol, get_info) == 64);
static_assert(__builtin_offsetof(efi_file_protocol, set_info) == 72);
static_assert(__builtin_offsetof(efi_file_protocol, flush) == 80);

/* Open modes and attributes of EFI_FILE_PROTOCOL.Open. A mode is READ, READ|WRITE or
 * READ|WRITE|CREATE; writing alone is not one. */
inline constexpr unsigned long long EFI_FILE_MODE_READ   = 0x0000000000000001ULL;
inline constexpr unsigned long long EFI_FILE_MODE_WRITE  = 0x0000000000000002ULL;
inline constexpr unsigned long long EFI_FILE_MODE_CREATE = 0x8000000000000000ULL;
inline constexpr unsigned long long EFI_FILE_READ_ONLY   = 0x0000000000000001ULL;
inline constexpr unsigned long long EFI_FILE_DIRECTORY   = 0x0000000000000010ULL;
/* SetPosition to this value places the position at the end of the file. */
inline constexpr unsigned long long EFI_FILE_POSITION_END = 0xFFFFFFFFFFFFFFFFULL;

/* EFI_FILE_INFO (UEFI 2.10, 13.5.16): what GetInfo answers of a node, and what a directory's Read
 * gives for each entry. The name follows the fixed part and ends in a NUL; `size' counts both. */
struct efi_file_info {
    unsigned long long size;              /*  0 */
    unsigned long long file_size;         /*  8 */
    unsigned long long physical_size;     /* 16 */
    efi_time           create_time;       /* 24 */
    efi_time           last_access_time;  /* 40 */
    efi_time           modification_time; /* 56 */
    unsigned long long attribute;         /* 72 */
    efi_char16         file_name[1];      /* 80 */
};
static_assert(__builtin_offsetof(efi_file_info, file_size) == 8);
static_assert(__builtin_offsetof(efi_file_info, modification_time) == 56);
static_assert(__builtin_offsetof(efi_file_info, attribute) == 72);
static_assert(__builtin_offsetof(efi_file_info, file_name) == 80, "SIZE_OF_EFI_FILE_INFO is 80");
inline constexpr efi_uintn kFileInfoFixed = 80;
inline constexpr efi_guid kFileInfo = {
    0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* EFI_FILE_SYSTEM_INFO (UEFI 2.10, 13.5.17): the volume a handle is on. */
struct efi_file_system_info {
    unsigned long long size;          /*  0 */
    efi_u8             read_only;     /*  8 */
    unsigned long long volume_size;   /* 16 */
    unsigned long long free_space;    /* 24 */
    efi_u32            block_size;    /* 32 */
    efi_char16         volume_label[1];  /* 36 */
};
static_assert(__builtin_offsetof(efi_file_system_info, read_only) == 8);
static_assert(__builtin_offsetof(efi_file_system_info, volume_size) == 16);
static_assert(__builtin_offsetof(efi_file_system_info, free_space) == 24);
static_assert(__builtin_offsetof(efi_file_system_info, volume_label) == 36);
inline constexpr efi_guid kFileSystemInfo = {
    0x09576E93, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* EFI_RNG_PROTOCOL (UEFI 2.10, 37.5): the firmware's source of random bytes. A null algorithm asks
 * for the source's default. */
struct efi_rng_protocol {
    efi_status (*get_info)(efi_rng_protocol*, efi_uintn* list_size, efi_guid* list);       /* 0 */
    efi_status (*get_rng)(efi_rng_protocol*, const efi_guid* algorithm, efi_uintn length,
                          efi_u8* value);                                                   /* 8 */
};
static_assert(__builtin_offsetof(efi_rng_protocol, get_rng) == 8);
inline constexpr efi_guid kRngProtocol = {
    0x3152BCA5, 0xEADE, 0x433D, {0x86, 0x2E, 0xC0, 0x1C, 0xDC, 0x29, 0x1F, 0x44}};

/* EFI_SHELL_PARAMETERS_PROTOCOL (UEFI Shell 2.2, 2.3): what the UEFI Shell installs on the image
 * handle of an application it starts --- the command line already divided into arguments by the
 * Shell's own rules (quotes and its escape character removed), each UCS-2. */
struct efi_shell_parameters_protocol {
    efi_char16** argv;      /*  0 */
    efi_uintn    argc;      /*  8 */
    void*        std_in;    /* 16 */
    void*        std_out;   /* 24 */
    void*        std_err;   /* 32 */
};
static_assert(__builtin_offsetof(efi_shell_parameters_protocol, argv) == 0);
static_assert(__builtin_offsetof(efi_shell_parameters_protocol, argc) == 8);
inline constexpr efi_guid kShellParametersProtocol = {
    0x752F3136, 0x4E16, 0x4FDC, {0xA2, 0x2A, 0xE5, 0xF4, 0x68, 0x12, 0xF4, 0xCA}};

#endif /* OPENKAL_UEFI_H */
