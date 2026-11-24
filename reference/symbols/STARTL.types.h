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

typedef struct CornerProbe {      /* 0x2c bytes, PlayerCar.probe[4] */
    unsigned char pad_00[0xc];
    int x;                        /* 0x0c world position of the corner */
    unsigned char pad_10[0x4];
    int z;                        /* 0x14 */
    int hit;                      /* 0x18 inside a wall */
    int push_x;                   /* 0x1c */
    unsigned char pad_20[0x4];
    int push_z;                   /* 0x24 */
    int f_28;                     /* 0x28 */
} CornerProbe;

typedef struct Wheel {            /* 0x54 bytes, PlayerCar.wheel[4] */
    int x;                        /* 0x00 */
    int y;                        /* 0x04 height = ground_y (+ bump) */
    int z;                        /* 0x08 */
    unsigned char pad_0c[0x18];
    int ground_y;                 /* 0x24 track height under the wheel */
    int f_28;                     /* 0x28 */
    int bumpy;                    /* 0x2c surface adds a small bump */
    int f_30;                     /* 0x30 */
    unsigned char pad_34[0x4];
    float load_lat;               /* 0x38 lateral load */
    unsigned char pad_3c[0x4];
    float load_long;              /* 0x40 longitudinal load */
    unsigned char pad_44[0x10];
} Wheel;

/* The human player's car (g_player_car). Size from the gap to the next global. */
typedef struct PlayerCar { /* 0x1084 bytes */
    int wheel_susp[4]; /* 0x000 suspension travel per wheel; lowers the wheel sub-objects */
    int frame_flags_010; /* 0x010 cleared every frame (copy of frame_flags_018) */
    unsigned char pad_014[0x4];
    int frame_flags_018; /* 0x018 cleared every frame */
    unsigned char pad_01c[0x24];
    int wall_contact; /* set when a corner probe hits a wall */
    int wall_push_x; /*  */
    unsigned char pad_048[0x4];
    int wall_push_z; /*  */
    int wall_probe_data; /* 0x050 f_28 of the probe that hit the wall */
    CornerProbe probe[4]; /* 0x054 wall probes at the four corners */
    unsigned int nearby_cpu_mask; /* bit n set when g_opponents[n] is within 1500 units */
    unsigned int crash_yaw_rate; /* 0x108 yaw spin while crashed; 0 when driving normally */
    unsigned char pad_10c[0x4];
    int braking; /* 0x110 brake as an int: non-zero while the brake is applied */
    float brake; /* 0..1 */
    int shadow_x; /* car_update_shadow_position */
    int shadow_y; /* pos_y - 20 */
    int shadow_z; /*  */
    float body_pitch_ofs; /* 0x124 added to pitch for drawing, ghosts and network */
    float drift_angle; /* 0x128 added to yaw for drawing; relaxes back to 0 (car_update_skid) */
    float body_roll_ofs; /* 0x12c added to roll for drawing */
    int body_ofs_x; /* added to pos_* for the body object */
    int body_ofs_y; /*  */
    int body_ofs_z; /*  */
    int track_heading; /* heading of the track at the car (track_find_nearest_point) */
    int gear_shift_latch; /* 0x140 one shift per key press */
    float throttle; /* 0..1 */
    int crash_state; /* 0x148: 0 driving, 2 just crashed, 1 crashed, 5 ... */
    int crash_bounce; /* 0x14c bounce speed after a crash, starts at 3000 */
    int crash_bounce_count; /* 0x150 crash ends after 4 bounces */
    int crash_pitch_rate; /* 0x154 */
    unsigned char pad_158[0x4];
    int crash_roll_rate; /* 0x15c */
    int crash_event_slot; /* 0x160 announcer slot set on a crash */
    unsigned char pad_164[0xc];
    int spin_state; /* 1 = spinning (car_update_spin) */
    float rpm_per_speed; /* 0x174 engine_rpm = sh_rap[gear-1] * speed * rpm_per_speed */
    float engine_f178; /* 0x178 read by car_update_engine_gearbox */
    float brake_decel; /* speed -= brake_decel * dt * brake */
    float slide_threshold; /* speed * heading error above this slides the car */
    float grip_limit; /* lateral force limit before skidding */
    float skid_brake_limit; /* 0x188 braking force above this skids */
    float max_accel; /* 0x18c speed gain per second limit */
    float brake_skid_limit; /* 0x190 brake * speed above this starts a skid */
    Wheel wheel[4]; /* 0x194 */
    unsigned char pad_2e4[0xc];
    int brake_mode; /* 0x2f0 car_update_brake takes another path when non-zero */
    float turn_rate; /* steering curvature; lateral load ~ turn_rate * speed^2 */
    float turn_radius_k; /* 0x2f8 lateral acceleration = speed^2 / turn_radius_k */
    unsigned int net_contact_mask; /* 0x2fc bit n set when touching network car n */
    unsigned char pad_300[0x14c];
    int wheel_part0; /* 0x44c sub-object index of a wheel inside the body model */
    unsigned char pad_450[0x4];
    int wheel_part1; /* 0x454 */
    unsigned char pad_458[0x4];
    int wheel_part2; /* 0x45c */
    int body_obj; /* index into g_objects */
    int wheel_part3; /* 0x464 */
    int shadow_obj; /* index into g_objects */
    unsigned char pad_46c[0x4];
    CarData car; /* 0x470 copy of the selected g_cars_data entry */
    int pos_x; /* world position, map units */
    int pos_y;
    int pos_z;
    int cam_shake_x; /* 0x4bc chase camera x offset, toggled while crashing */
    int body_lift; /* 0x4c0 raises the body and chase camera, lowers the wheels */
    unsigned char pad_4c4[0x4];
    float pitch; /* radians */
    float yaw; /* radians, wrapped to 0..2pi */
    float roll; /* radians */
    float ground_pitch; /* 0x4d4 from the wheel heights (car_update_body_from_wheels) */
    unsigned char pad_4d8[0x4];
    float ground_pitch_neg; /* 0x4dc */
    float lean_pitch; /* 0x4e0 from wheel loads (car_update_wheel_loads) */
    unsigned char pad_4e4[0x4];
    float lean_roll; /* 0x4e8 */
    int ground_y; /* average ground height under the wheels */
    int skid_prev; /* 0x4f0 */
    unsigned int skid; /* 0x4f4 non-zero while skidding */
    unsigned char pad_4f8[0x4];
    float skid_level; /* 0x4fc decays to 0 */
    float slip_a; /* 0x500 */
    unsigned char pad_504[0x4];
    float slip_b; /* 0x508 */
    float speed;
    float speed_limit; /* 0x510 top speed for the current gear and surface */
    unsigned char pad_514[0x18];
    int front_bumper_x; /* world space (car_update_bumpers) */
    unsigned char pad_530[0x4];
    int front_bumper_z; /*  */
    int rear_bumper_x; /*  */
    unsigned char pad_53c[0x4];
    int rear_bumper_z; /*  */
    int collision_radius; /* used for all car/car and car/obstacle tests */
    float yaw_rate; /* rad/s, decays each frame */
    float impact_timer; /* 0x54c 5000 after a hit, decays */
    float impact_vel_x; /* set by collisions */
    unsigned char pad_554[0x4];
    float impact_vel_z; /*  */
    int contact_dist; /* distance to the car being tested */
    int contact_limit; /* 0x560 sum of the two collision radii */
    int contact_dx; /*  */
    unsigned char pad_568[0x4];
    int contact_dz; /*  */
    unsigned char pad_570[0x18];
    float prev_speed; /* speed last frame */
    int prev_surface; /* 0x58c */
    int prev_marker_flags; /* 0x590 */
    unsigned char pad_594[0x4];
    int track_marker; /* nearest track marker index */
    int checkpoints_passed; /*  */
    unsigned char pad_5a0[0xc8];
    int checkpoint_marker; /* 0x668 track marker index for checkpoint logic */
    unsigned int surface; /* 0x66c surface type from the track marker; picks engine sample and dust flag (car_apply_surface) */
    float surface_grip; /* 0x670 from the track marker table */
    unsigned char pad_674[0x4];
    int marker_flags; /* 0x678 bit 1 checkpoint, bit 2 announcer cue */
    float marker_fraction; /* 0x67c progress between this marker and the next */
    float track_progress; /* 0x680 marker_fraction + marker + lap * 100 (FSTP float); used for race positions */
    int lap; /* laps completed */
    int race_position; /* 1 = first; also used as result code in some modes */
    unsigned char pad_68c[0xd0];
    int marker_index; /* 0x75c checkpoint mode */
    unsigned char pad_760[0x20];
    int checkpoint_event; /* 0x780 1 passed a checkpoint, 2 passed the line */
    int timing_started; /* 0x784 lap timing runs once the line has been crossed */
    unsigned char pad_788[0x4];
    int lap_time; /* s */
    float time_left; /* checkpoint timer; +bonus_time per checkpoint, +1 per obstacle */
    float race_time; /* s */
    unsigned char pad_798[0x10];
    float engine_rpm; /* (unconfirmed) engine gearbox and cockpit gauges */
    int gear; /* (unconfirmed)  */
    int surface_render_flag; /* 0x7b0 0x8000 off-road (dust); passed to the body object */
    unsigned int render_flags; /* CarData.animation | 0x4000 no brake | 0x2000 slow */
    int obstacles_hit; /* count */
    int obstacles_counted; /* car_add_obstacle_time */
    int markers_passed; /* checkpoint mode */
    unsigned char pad_7c4[0x8];
    int engine_sfx; /* sample handle of the engine sound */
    int skid_sfx; /* 0x7d0 sample handle, -1 when silent */
    int surface_sfx; /* 0x7d4 */
    unsigned char pad_7d8[0x10];
    int net_session; /* 0x7e8 network packet sent by net_pack_player_state */
    int net_node; /* 0x7ec */
    unsigned int net_flags; /* 0x7f0 render flags */
    int net_surface_flag; /* 0x7f4 */
    int net_car_index; /* 0x7f8 */
    int net_lap; /* 0x7fc */
    int net_position; /* 0x800 */
    int net_progress; /* 0x804 */
    int net_lap_time; /* 0x808 */
    int net_impact_x; /* 0x80c */
    unsigned char pad_810[0x4];
    int net_impact_z; /* 0x814 */
    int net_x; /* 0x818 */
    int net_y; /* 0x81c */
    int net_z; /* 0x820 */
    float net_pitch; /* 0x824 */
    float net_yaw; /* 0x828 */
    float net_roll; /* 0x82c */
    int net_ground_y; /* 0x830 */
    int net_pitch_i; /* 0x834 */
    int net_roll_i; /* 0x838 */
    unsigned char pad_83c[0x848];
} PlayerCar;

/* A CPU-driven car (g_opponents[]). The OPPONENTS section is loaded at +0x2e4. */
typedef struct CpuCar { /* 0x590 bytes */
    unsigned char pad_000[0x4];
    int shadow_obj; /* index into g_objects */
    unsigned char pad_008[0x4];
    int nearby_cpu_mask; /* bit n set when a later opponent is within 1500 units */
    float unused_010; /* 0x010 no reads or writes found */
    unsigned char pad_014[0x44];
    int wheel_part1; /* 0x058 sub-object index; wheel blocks are 0x54 apart */
    unsigned char pad_05c[0x50];
    int wheel_part2; /* 0x0ac */
    unsigned char pad_0b0[0x50];
    int wheel_part3; /* 0x100 */
    unsigned char pad_104[0x50];
    int wheel_part4; /* 0x154 */
    unsigned char pad_158[0x8];
    int prev_lateral_x; /* 0x160 opponent_save_prev_position */
    unsigned char pad_164[0x4];
    int prev_lateral_z; /* 0x168 */
    int prev_x; /* 0x16c */
    unsigned char pad_170[0x4];
    int prev_z; /* 0x174 */
    int prev_speed; /* 0x178 a sudden drop marks a hit (opponent_find_cpu_contacts) */
    unsigned char pad_17c[0x6c];
    int body_front_z; /* 0x1e8 used to place the bumpers */
    unsigned char pad_1ec[0x28];
    int body_rear_z; /* 0x214 */
    unsigned char pad_218[0x20];
    int ground_y; /* 0x238 track height on the racing line (opponent_check_ground) */
    float ground_pitch; /* 0x23c */
    unsigned char pad_240[0x4];
    float ground_roll; /* 0x244 */
    int spline_index; /* racing line */
    int spline_segment; /*  */
    int spline_t; /* position within the current spline segment */
    int spline_step; /* 0x254 0 or 1: which half of the segment */
    int line_x; /* 0x258 racing-line position; pos = line + lateral offset */
    int line_point; /* 0x25c current spline point index */
    int line_z; /* 0x260 */
    int ctrl_a_x; /* 0x264 spline control points */
    unsigned char pad_268[0x4];
    int ctrl_a_z; /* 0x26c */
    int ctrl_b_x; /* 0x270 */
    unsigned char pad_274[0x4];
    int ctrl_b_z; /* 0x278 */
    float line_yaw; /* 0x27c heading of the racing line */
    int line_yaw_prev; /* 0x280 */
    float seg_p0x; /* 0x284 segment control points (floats) */
    float seg_p0z; /* 0x288 */
    unsigned char pad_28c[0x4];
    float seg_p1x; /* 0x290 */
    float seg_p1z; /* 0x294 */
    unsigned char pad_298[0x4];
    float seg_p2x; /* 0x29c */
    float seg_p2z; /* 0x2a0 */
    unsigned char pad_2a4[0x10];
    int ctrl_next_x; /* 0x2b4 */
    int ctrl_next_z; /* 0x2b8 */
    unsigned char pad_2bc[0x4];
    int catching_up; /* 0x2c0 set while the target speed is raised to catch the player */
    int shadow_y; /* 0x2c4 */
    int part_front_wheels; /* 0x2c8 "fauto" sub-object */
    int part_front_angle; /* 0x2cc */
    int part_rear_wheels; /* 0x2d0 "rauto" sub-object */
    int part_rear_angle; /* 0x2d4 */
    int part_steer; /* 0x2d8 "MV_mauto" sub-object */
    int part_steer_angle; /* 0x2dc */
    int car_index; /* 0x2e0 index into g_cars_data */
    OpponentDef def; /* 0x2e4 from the track's OPPONENTS section */
    int crash_state; /* 0 driving, 2 just crashed, 1 crashed */
    int crash_bounce; /* 0x2fc starts at 3000 */
    int crash_bounce_count; /* 0x300 */
    int crash_pitch_rate; /* 0x304 */
    int crash_yaw_rate; /* 0x308 */
    int crash_roll_rate; /* 0x30c */
    unsigned char pad_310[0x4];
    float crash_pitch; /* 0x314 added to pitch */
    float crash_yaw; /* 0x318 added to yaw */
    float crash_roll; /* 0x31c added to roll */
    int track_marker; /* nearest track marker */
    int checkpoints_passed; /* 0x324 */
    unsigned char pad_328[0xc8];
    int checkpoint_marker; /* 0x3f0 */
    int marker_flags; /* 0x3f4 */
    unsigned char pad_3f8[0x4];
    float speed_factor; /* 0x3fc multiplies OpponentDef speeds */
    unsigned char pad_400[0x4];
    float marker_fraction; /* 0x404 */
    float track_progress; /* 0x408 marker_fraction + marker + lap * 100 */
    int lap; /* 0x40c */
    int race_position; /*  */
    int active;
    float speed; /*  */
    int front_bumper_local_x; /* 0x41c body space */
    unsigned char pad_420[0x4];
    int front_bumper_local_z; /* 0x424 */
    int rear_bumper_local_x; /* 0x428 */
    unsigned char pad_42c[0x4];
    int rear_bumper_local_z; /* 0x430 */
    int front_bumper_x; /* world space */
    unsigned char pad_438[0x4];
    int front_bumper_z; /*  */
    int rear_bumper_x; /*  */
    unsigned char pad_444[0x4];
    int rear_bumper_z; /*  */
    int collision_radius; /*  */
    float lateral_push; /* 0x450 decays each frame */
    int lateral_ofs_x; /* offset from the racing line */
    int lateral_start; /* 0x458 */
    int lateral_ofs_z; /*  */
    unsigned char pad_460[0xc];
    float lateral_timer; /* 0x46c */
    unsigned char pad_470[0x14];
    int contact_dist; /* 0x484 */
    int contact_limit; /* 0x488 sum of radii */
    int contact_dx; /* 0x48c */
    unsigned char pad_490[0x4];
    int contact_dz; /* 0x494 */
    int pos_x; /* world position */
    int pos_y;
    int pos_z;
    float pitch; /* radians */
    float yaw; /* radians */
    float roll; /* radians */
    unsigned char pad_4b0[0xc];
    float draw_yaw; /* 0x4bc yaw + yaw_wobble */
    unsigned char pad_4c0[0x8];
    float yaw_wobble; /* 0x4c8 relaxes to 0 */
    unsigned char pad_4cc[0x10];
    int body_obj; /* index into g_objects */
    int part_extra; /* 0x4e0 sub-object index */
    float viewer_distance; /* 0x4e4 the car is simulated only while this is small enough */
    int render_flags; /* 0x4e8 0 or 0xff00 (brake lights) */
    int engine_sfx; /* 0x4ec sample handle, -1 when silent */
    unsigned char pad_4f0[0x8];
    int tv_cam_x; /* 0x4f8 camera_tv_track_opponent state */
    int tv_cam_y; /* 0x4fc */
    int tv_cam_z; /* 0x500 */
    int tv_cam_target_x; /* 0x504 */
    unsigned char pad_508[0x4];
    int tv_cam_target_z; /* 0x50c */
    unsigned char pad_510[0x18];
    int tv_cam_angle; /* 0x528 */
    int tv_cam_dist; /* 0x52c */
    int tv_cam_flag; /* 0x530 */
    unsigned char pad_534[0x18];
    int tv_cam_a; /* 0x54c */
    int tv_cam_b; /* 0x550 */
    int tv_cam_point; /* 0x554 nearest TV camera point */
    int tv_cam_prev_point; /* 0x558 */
    unsigned char pad_55c[0x8];
    int tv_cam_cut; /* 0x564 */
    int tv_cam_target; /* 0x568 */
    int tv_cam_c; /* 0x56c */
    unsigned char pad_570[0x4];
    int tv_cam_d; /* 0x574 */
    int tv_cam_e; /* 0x578 */
    unsigned char pad_57c[0x10];
    int tv_cam_car_id; /* 0x58c */
} CpuCar;

/* A network player's car (g_net_players[]): filled from the packets other players send
 * (see the net_* fields of PlayerCar). */
typedef struct NetCar { /* 0x2a8 bytes */
    unsigned char pad_000[0x14];
    int flags; /* 0x014 bit per node, 0x10000 crashed, 0x20000 left the race */
    int render_flags; /* 0x018 passed to the body object */
    int car_index; /* 0x01c index into g_cars_data (0..12) */
    unsigned char pad_020[0x1c];
    int pos_x; /* 0x03c */
    int pos_y; /* 0x040 */
    int pos_z; /* 0x044 */
    unsigned char pad_048[0x4];
    float yaw; /* 0x04c */
    unsigned char pad_050[0x4];
    int shadow_y; /* 0x054 */
    unsigned char pad_058[0xc];
    int body_obj; /* 0x064 index into g_objects */
    unsigned char pad_068[0x4];
    int shadow_obj; /* 0x06c */
    unsigned char pad_070[0xb4];
    unsigned char pad_124[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_128[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_12c[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_130[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_134[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_138[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_13c[0xc];
    unsigned char pad_148[0x4]; /* draft field from a PlayerCar function; not a NetCar access */
    unsigned char pad_14c[0xd8];
    int body_front_z; /* 0x224 used to place the bumpers */
    unsigned char pad_228[0x28];
    int body_rear_z; /* 0x250 */
    unsigned char pad_254[0x20];
    int front_bumper_local_x; /* 0x274 body space */
    unsigned char pad_278[0x4];
    int front_bumper_local_z; /* 0x27c */
    int rear_bumper_local_x; /* 0x280 */
    unsigned char pad_284[0x4];
    int rear_bumper_local_z; /* 0x288 */
    int front_bumper_x; /* 0x28c world space */
    unsigned char pad_290[0x4];
    int front_bumper_z; /* 0x294 */
    int rear_bumper_x; /* 0x298 */
    unsigned char pad_29c[0x4];
    int rear_bumper_z; /* 0x2a0 */
    int collision_radius; /* 0x2a4 */
} NetCar;

#pragma pack(pop)
