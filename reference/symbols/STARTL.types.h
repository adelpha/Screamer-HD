/* Data types recovered from STARTL.EXE (Screamer, 1995). Parsed by Ghidra's C parser
 * (tools/ghidra/scripts/ApplySymbols.java) before reference/symbols/STARTL.sym is applied.
 *
 * Every struct is packed; offsets are given in comments where fields are named.
 * Fields called f_<offset> have a known type (from how the code accesses them) but no known
 * meaning yet; pad_<offset> are bytes no analysed function touches. Rename fields here as
 * their meaning is established. tools/ghidra/field_usage.py drafts new structs from the dump.
 */
#pragma pack(push, 1)

/* ---- Watcom C runtime ---- */

typedef struct WatcomFILE {       /* 0x1a bytes; __iob is an array of these */
    unsigned char *ptr;           /* 0x00 next byte in the buffer */
    int cnt;                      /* 0x04 bytes left in the buffer */
    unsigned char *base;          /* 0x08 buffer */
    unsigned int flag;            /* 0x0c _READ 1, _WRITE 2, _EOF 0x10, _SFERR 0x20, _BINARY 0x40 ... */
    int handle;                   /* 0x10 DOS file handle */
    int bufsize;                  /* 0x14 */
    unsigned char ungotten;       /* 0x18 */
    unsigned char tmpfchar;       /* 0x19 */
} WatcomFILE;

typedef struct DWORDREGS {        /* union REGS (32-bit view) for int386/int386x */
    unsigned int eax, ebx, ecx, edx, esi, edi;
    unsigned int cflag;
} DWORDREGS;

typedef struct SREGS {
    unsigned short es, cs, ss, ds, fs, gs;
} SREGS;

typedef struct DpmiRealModeCall { /* DPMI 0300h register block (0x32 bytes) */
    unsigned int edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} DpmiRealModeCall;

/* ---- memory manager (C++ class) ---- */

typedef struct MemSlot {          /* 0x1c bytes */
    char name[12];                /* 0x00 "%.11s" of the allocation name */
    void *ptr;                    /* 0x0c */
    unsigned int handle;          /* 0x10 DPMI memory handle or DOS selector */
    int size;                     /* 0x14 */
    int is_dos;                   /* 0x18 1 = conventional memory (DPMI 0100h) */
} MemSlot;

typedef struct MemMgr {           /* 0x4c bytes */
    MemSlot *slots;               /* 0x00 */
    int count;                    /* 0x04 */
    int capacity;                 /* 0x08 */
    int error;                    /* 0x0c 1 out of memory, 2 no slot, 3 not found, 7/9 corrupted */
    char error_name[12];          /* 0x10 */
    unsigned int dpmi_info[12];   /* 0x1c DPMI 0500h free memory information */
} MemMgr;

/* ---- data archives (*.DAT) ---- */

typedef struct ArchiveDirEntry {  /* 0x20 bytes, stored XOR 0x3A at the end of the .DAT */
    char name[16];                /* 0x00 */
    int offset;                   /* 0x10 */
    int f_14;                     /* 0x14 passed to the LZW decoder */
    int size;                     /* 0x18 bytes available to archive_read */
    int flags;                    /* 0x1c bit0 kept in memory, bit1 LZW compressed */
} ArchiveDirEntry;

typedef struct ArchiveFile {      /* 0xb4 bytes ("ALFILE"), lives on the caller's stack */
    WatcomFILE *fp;               /* 0x00 */
    int start;                    /* 0x04 file offset of the entry */
    int end;                      /* 0x08 */
    int pos;                      /* 0x0c */
    int compressed;               /* 0x10 */
    int in_memory;                /* 0x14 */
    int mem_pos;                  /* 0x18 */
    unsigned char *mem;           /* 0x1c whole entry when in_memory */
    char mode[4];                 /* 0x20 */
    char path[52];                /* 0x24 */
    ArchiveDirEntry entry;        /* 0x58 */
    char error_name[52];          /* 0x78 */
    int error;                    /* 0xac 1 cannot open, 2 not found, 3 out of memory, 4 out of range, 5 no decode memory */
    void *lzw;                    /* 0xb0 LZW state (0x8490 bytes) */
} ArchiveFile;

/* ---- 3D engine object manager ---- */

typedef struct ObjSlot {          /* 0x1c bytes, g_objects[120] */
    char name[12];                /* 0x00 object (.3DX) name */
    void *instance;               /* 0x0c 3D_BLKOBJ instance */
    void *f_10;                   /* 0x10 */
    void *model;                  /* 0x14 shared 3D_HEADER (duplicates point at the same one) */
    int visual_index;             /* 0x18 position in the visual list, -1 when hidden */
} ObjSlot;

/* ---- track objects ---- */

typedef struct ObstacleInstance { /* 0x40 bytes, g_obstacles[] (placed from the track's obstacle list) */
    int x;                        /* 0x00 world position */
    int y;                        /* 0x04 */
    int z;                        /* 0x08 */
    unsigned char pad_0c[0xc];
    int obj;                      /* 0x18 index into g_objects */
    unsigned char pad_1c[0x4];
    int moving;                   /* 0x20 */
    int vel_x;                    /* 0x24 set from sin(yaw) when a car hits it */
    unsigned char pad_28[0x4];
    int vel_z;                    /* 0x2c set from -cos(yaw) */
    int f_30;                     /* 0x30 from car speed */
    int f_34;                     /* 0x34 from car speed */
    int hit;                      /* 0x38 0 = still standing */
    int flying;                   /* 0x3c */
} ObstacleInstance;

/* ---- cars ---- */

/* Records loaded from the text .DTR files in the .DAT archives (data_load_section). Each
 * field in a .DTR line is prefixed by its type: s = char[12], d = int, x = hex int, f = float.
 * Field names below are the ones in the .DTR header comments. */

typedef struct CarData {          /* 0x40 bytes, CARS_DATA in GEN.DAT CARS.DTR, g_cars_data[] */
    char filename[12];            /* 0x00 car object, e.g. "AUTO00" */
    unsigned int animation;       /* 0x0c 0 or 0x8000 (second paint scheme of the same model) */
    float maxspd;                 /* 0x10 */
    float maxrpm;                 /* 0x14 */
    float grip;                   /* 0x18 */
    float accel;                  /* 0x1c */
    int shiftmode;                /* 0x20 */
    int n_shifts;                 /* 0x24 number of gears */
    float sh_rap[6];              /* 0x28 gear ratios */
} CarData;

typedef struct OpponentDef {      /* 0x14 bytes, OPPONENTS in data\mapN.dat mapN[a-e].dtr */
    float max_spd;
    float accel;
    float brakes;
    float skill;
    float sband;
} OpponentDef;

typedef struct TrackData {        /* 0x10 bytes, TRACK_DATA in mapN[a-e].dtr */
    int color;
    int camera_hi;
    int bonus_time;
    int carnage;
} TrackData;

typedef struct ObstacleDef {      /* 0x24 bytes, OSTACLES in mapNdat.dtr */
    char name[3][12];             /* 3dx_name per detail level, picked by g_display_flags & 3 */
} ObstacleDef;

typedef struct AnimationDef {     /* 0xbc bytes, ANIMATIONS in mapNdat.dtr (first 0x18 from the file) */
    char name[12];
    float speed;
    int spln;
    int sound_fx;
    unsigned char runtime[0xa4];
} AnimationDef;

typedef struct SoundFxDef {       /* 0x24 bytes, SOUND_FX in several .dtr files */
    char name[12];                /* <name>.RAW in data\sound.dat */
    unsigned int volume;          /* 0x7fff = full */
    int loop;                     /* -1 = loop forever */
    int loop_start;
    int loop_leng;
    unsigned int pitch;           /* 16.16, 0x10000 = normal */
    unsigned int pan;             /* 0x01000000 = centre */
} SoundFxDef;

/* The human player's car (g_player_car). Size from the gap to the next global. */
typedef struct PlayerCar { /* 0x1084 bytes */
    unsigned char pad_000[0x4];
    int f_004;
    int f_008;
    int f_00c;
    int f_010;
    unsigned char pad_014[0x4];
    int f_018;
    unsigned char pad_01c[0x24];
    int f_040;
    int f_044;
    unsigned char pad_048[0x4];
    int f_04c;
    int f_050;
    unsigned char pad_054[0x18];
    int f_06c;
    unsigned char pad_070[0x54];
    int f_0c4;
    unsigned char pad_0c8[0x3c];
    unsigned int nearby_cpu_mask; /* bit n set when g_opponents[n] is within 1500 units */
    unsigned int f_108;
    unsigned char pad_10c[0x4];
    int f_110;
    float brake; /* 0..1 */
    int f_118;
    int f_11c;
    int f_120;
    float f_124;
    float f_128;
    float f_12c;
    int f_130;
    int f_134;
    int f_138;
    int f_13c;
    int f_140;
    float throttle; /* 0..1 */
    int crash_state; /* 0x148: 0 driving, 2 just crashed, 1 crashed, 5 ... */
    int f_14c;
    int f_150;
    int f_154;
    unsigned char pad_158[0x4];
    int f_15c;
    int f_160;
    unsigned char pad_164[0xc];
    int f_170;
    float f_174;
    float f_178;
    float f_17c;
    float f_180;
    float f_184;
    float f_188;
    float f_18c;
    float f_190;
    unsigned char pad_194[0x4];
    int f_198;
    unsigned char pad_19c[0x1c];
    int f_1b8;
    unsigned char pad_1bc[0x4];
    int f_1c0;
    unsigned char pad_1c4[0x4];
    int f_1c8;
    unsigned char pad_1cc[0x10];
    int f_1dc;
    unsigned char pad_1e0[0x4];
    int f_1e4;
    unsigned char pad_1e8[0x4];
    int f_1ec;
    unsigned char pad_1f0[0x1c];
    int f_20c;
    unsigned char pad_210[0x4];
    int f_214;
    unsigned char pad_218[0x4];
    int f_21c;
    unsigned char pad_220[0x10];
    int f_230;
    unsigned char pad_234[0xc];
    int f_240;
    unsigned char pad_244[0x1c];
    int f_260;
    unsigned char pad_264[0x4];
    int f_268;
    unsigned char pad_26c[0x4];
    int f_270;
    unsigned char pad_274[0x10];
    int f_284;
    unsigned char pad_288[0x4];
    int f_28c;
    unsigned char pad_290[0x4];
    int f_294;
    unsigned char pad_298[0x1c];
    int f_2b4;
    unsigned char pad_2b8[0x4];
    int f_2bc;
    unsigned char pad_2c0[0x4];
    int f_2c4;
    unsigned char pad_2c8[0x10];
    int f_2d8;
    unsigned char pad_2dc[0x14];
    int f_2f0;
    float f_2f4;
    float f_2f8;
    unsigned int f_2fc;
    unsigned char pad_300[0x14c];
    int f_44c;
    unsigned char pad_450[0x4];
    int f_454;
    unsigned char pad_458[0x4];
    int f_45c;
    int body_obj; /* index into g_objects */
    int f_464;
    int shadow_obj; /* index into g_objects */
    unsigned char pad_46c[0x4];
    CarData car; /* 0x470 copy of the selected g_cars_data entry */
    int pos_x; /* world position, map units */
    int pos_y;
    int pos_z;
    int f_4bc;
    int f_4c0;
    unsigned char pad_4c4[0x4];
    float pitch; /* radians */
    float yaw; /* radians, wrapped to 0..2pi */
    float roll; /* radians */
    float f_4d4;
    unsigned char pad_4d8[0x4];
    float f_4dc;
    float f_4e0;
    unsigned char pad_4e4[0x4];
    float f_4e8;
    int f_4ec;
    int f_4f0;
    unsigned int f_4f4;
    unsigned char pad_4f8[0x4];
    float f_4fc;
    float f_500;
    unsigned char pad_504[0x4];
    float f_508;
    float speed;
    float f_510;
    unsigned char pad_514[0x18];
    int f_52c;
    unsigned char pad_530[0x4];
    int f_534;
    int f_538;
    unsigned char pad_53c[0x4];
    int f_540;
    int f_544;
    float yaw_rate; /* rad/s, decays each frame */
    float f_54c;
    float f_550;
    unsigned char pad_554[0x4];
    float f_558;
    int f_55c;
    int f_560;
    int f_564;
    unsigned char pad_568[0x4];
    int f_56c;
    unsigned char pad_570[0x18];
    float prev_speed; /* speed last frame */
    int f_58c;
    int f_590;
    unsigned char pad_594[0x4];
    int track_marker; /* nearest track marker index */
    int f_59c;
    unsigned char pad_5a0[0xc8];
    int f_668;
    unsigned int f_66c;
    float f_670;
    unsigned char pad_674[0x4];
    int f_678;
    float f_67c;
    int f_680;
    int lap; /* laps completed */
    int f_688;
    unsigned char pad_68c[0xd0];
    int f_75c;
    unsigned char pad_760[0x20];
    int f_780;
    int f_784;
    unsigned char pad_788[0x4];
    int lap_time; /* s */
    float time_left; /* checkpoint timer; +bonus_time per checkpoint, +1 per obstacle */
    float race_time; /* s */
    unsigned char pad_798[0x10];
    float f_7a8;
    int f_7ac;
    int f_7b0;
    unsigned int render_flags; /* CarData.animation | 0x4000 no brake | 0x2000 slow */
    int obstacles_hit; /* count */
    int f_7bc;
    int f_7c0;
    unsigned char pad_7c4[0x8];
    int f_7cc;
    int f_7d0;
    int f_7d4;
    unsigned char pad_7d8[0x10];
    int f_7e8;
    int f_7ec;
    unsigned int f_7f0;
    int f_7f4;
    int f_7f8;
    int f_7fc;
    int f_800;
    int f_804;
    int f_808;
    int f_80c;
    unsigned char pad_810[0x4];
    int f_814;
    int f_818;
    int f_81c;
    int f_820;
    float f_824;
    float f_828;
    float f_82c;
    int f_830;
    int f_834;
    int f_838;
    unsigned char pad_83c[0x848];
} PlayerCar;

/* A CPU-driven car (g_opponents[]). The OPPONENTS section is loaded at +0x2e4. */
typedef struct CpuCar { /* 0x590 bytes */
    unsigned char pad_000[0x4];
    int shadow_obj; /* index into g_objects */
    unsigned char pad_008[0x4];
    int nearby_cpu_mask; /* bit n set when a later opponent is within 1500 units */
    float f_010;
    unsigned char pad_014[0x44];
    int f_058;
    unsigned char pad_05c[0x50];
    int f_0ac;
    unsigned char pad_0b0[0x50];
    int f_100;
    unsigned char pad_104[0x50];
    int f_154;
    unsigned char pad_158[0x8];
    int f_160;
    unsigned char pad_164[0x4];
    int f_168;
    int f_16c;
    unsigned char pad_170[0x4];
    int f_174;
    int f_178;
    unsigned char pad_17c[0x6c];
    int f_1e8;
    unsigned char pad_1ec[0x28];
    int f_214;
    unsigned char pad_218[0x20];
    int f_238;
    float f_23c;
    unsigned char pad_240[0x4];
    float f_244;
    int f_248;
    int f_24c;
    int spline_t; /* position within the current spline segment */
    int f_254;
    int f_258;
    int f_25c;
    int f_260;
    int f_264;
    unsigned char pad_268[0x4];
    int f_26c;
    int f_270;
    unsigned char pad_274[0x4];
    int f_278;
    float f_27c;
    int f_280;
    float f_284;
    float f_288;
    unsigned char pad_28c[0x4];
    float f_290;
    float f_294;
    unsigned char pad_298[0x4];
    float f_29c;
    float f_2a0;
    unsigned char pad_2a4[0x10];
    int f_2b4;
    int f_2b8;
    unsigned char pad_2bc[0x4];
    int f_2c0;
    int f_2c4;
    int f_2c8;
    int f_2cc;
    int f_2d0;
    int f_2d4;
    int f_2d8;
    int f_2dc;
    int f_2e0;
    OpponentDef def; /* 0x2e4 from the track's OPPONENTS section */
    int crash_state; /* 0 driving, 2 just crashed, 1 crashed */
    int f_2fc;
    int f_300;
    int f_304;
    int f_308;
    int f_30c;
    unsigned char pad_310[0x4];
    float f_314;
    float f_318;
    float f_31c;
    int f_320;
    int f_324;
    unsigned char pad_328[0xc8];
    int f_3f0;
    int f_3f4;
    unsigned char pad_3f8[0x4];
    float f_3fc;
    unsigned char pad_400[0x4];
    float f_404;
    float f_408;
    int f_40c;
    int race_position; /*  */
    int active;
    float speed; /*  */
    int f_41c;
    unsigned char pad_420[0x4];
    int f_424;
    int f_428;
    unsigned char pad_42c[0x4];
    int f_430;
    int f_434;
    unsigned char pad_438[0x4];
    int f_43c;
    int f_440;
    unsigned char pad_444[0x4];
    int f_448;
    int f_44c;
    float f_450;
    int f_454;
    int f_458;
    int f_45c;
    unsigned char pad_460[0xc];
    float f_46c;
    unsigned char pad_470[0x14];
    int f_484;
    int f_488;
    int f_48c;
    unsigned char pad_490[0x4];
    int f_494;
    int pos_x; /* world position */
    int pos_y;
    int pos_z;
    float pitch; /* radians */
    float yaw; /* radians */
    float roll; /* radians */
    unsigned char pad_4b0[0xc];
    float f_4bc;
    unsigned char pad_4c0[0x8];
    float f_4c8;
    unsigned char pad_4cc[0x10];
    int body_obj; /* index into g_objects */
    int f_4e0;
    float f_4e4;
    int f_4e8;
    int f_4ec;
    unsigned char pad_4f0[0x8];
    int f_4f8;
    int f_4fc;
    int f_500;
    int f_504;
    unsigned char pad_508[0x4];
    int f_50c;
    unsigned char pad_510[0x18];
    int f_528;
    int f_52c;
    int f_530;
    unsigned char pad_534[0x18];
    int f_54c;
    int f_550;
    int f_554;
    int f_558;
    unsigned char pad_55c[0x8];
    int f_564;
    int f_568;
    int f_56c;
    unsigned char pad_570[0x4];
    int f_574;
    int f_578;
    unsigned char pad_57c[0x10];
    int f_58c;
} CpuCar;

/* A network player's car (g_net_players[]). */
typedef struct NetCar { /* 0x2a8 bytes */
    unsigned char pad_000[0x14];
    int f_014;
    int f_018;
    int f_01c;
    unsigned char pad_020[0x1c];
    int f_03c;
    int f_040;
    int f_044;
    unsigned char pad_048[0x4];
    float f_04c;
    unsigned char pad_050[0x4];
    int f_054;
    unsigned char pad_058[0xc];
    int f_064;
    unsigned char pad_068[0x4];
    int f_06c;
    unsigned char pad_070[0xb4];
    float f_124;
    float f_128;
    float f_12c;
    int f_130;
    int f_134;
    int f_138;
    unsigned char pad_13c[0xc];
    int f_148;
    unsigned char pad_14c[0xd8];
    int f_224;
    unsigned char pad_228[0x28];
    int f_250;
    unsigned char pad_254[0x20];
    int f_274;
    unsigned char pad_278[0x4];
    int f_27c;
    int f_280;
    unsigned char pad_284[0x4];
    int f_288;
    int f_28c;
    unsigned char pad_290[0x4];
    int f_294;
    int f_298;
    unsigned char pad_29c[0x4];
    int f_2a0;
    int f_2a4;
} NetCar;

#pragma pack(pop)
