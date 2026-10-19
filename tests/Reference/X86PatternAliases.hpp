/* Test-only aliases for expanding the original C declaration table. */
constexpr Code MC_MOV = Code::Mov;
constexpr Code MC_FMOV = Code::FMov;
constexpr Code MC_DMOV = Code::DMov;
constexpr Code MC_LDMOV = Code::LDMov;
constexpr Code MC_EXT8 = Code::Ext8;
constexpr Code MC_EXT16 = Code::Ext16;
constexpr Code MC_EXT32 = Code::Ext32;
constexpr Code MC_UEXT8 = Code::UExt8;
constexpr Code MC_UEXT16 = Code::UExt16;
constexpr Code MC_UEXT32 = Code::UExt32;
constexpr Code MC_I2F = Code::I2F;
constexpr Code MC_I2D = Code::I2D;
constexpr Code MC_I2LD = Code::I2LD;
constexpr Code MC_UI2F = Code::UI2F;
constexpr Code MC_UI2D = Code::UI2D;
constexpr Code MC_UI2LD = Code::UI2LD;
constexpr Code MC_F2I = Code::F2I;
constexpr Code MC_D2I = Code::D2I;
constexpr Code MC_LD2I = Code::LD2I;
constexpr Code MC_F2D = Code::F2D;
constexpr Code MC_F2LD = Code::F2LD;
constexpr Code MC_D2F = Code::D2F;
constexpr Code MC_D2LD = Code::D2LD;
constexpr Code MC_LD2F = Code::LD2F;
constexpr Code MC_LD2D = Code::LD2D;
constexpr Code MC_NEG = Code::Neg;
constexpr Code MC_NEGS = Code::Negs;
constexpr Code MC_FNEG = Code::FNeg;
constexpr Code MC_DNEG = Code::DNeg;
constexpr Code MC_LDNEG = Code::LDNeg;
constexpr Code MC_ADDR = Code::Addr;
constexpr Code MC_ADDR8 = Code::Addr8;
constexpr Code MC_ADDR16 = Code::Addr16;
constexpr Code MC_ADDR32 = Code::Addr32;
constexpr Code MC_ADD = Code::Add;
constexpr Code MC_ADDS = Code::Adds;
constexpr Code MC_FADD = Code::FAdd;
constexpr Code MC_DADD = Code::DAdd;
constexpr Code MC_LDADD = Code::LDAdd;
constexpr Code MC_SUB = Code::Sub;
constexpr Code MC_SUBS = Code::Subs;
constexpr Code MC_FSUB = Code::FSub;
constexpr Code MC_DSUB = Code::DSub;
constexpr Code MC_LDSUB = Code::LDSub;
constexpr Code MC_MUL = Code::Mul;
constexpr Code MC_MULS = Code::Muls;
constexpr Code MC_FMUL = Code::FMul;
constexpr Code MC_DMUL = Code::DMul;
constexpr Code MC_LDMUL = Code::LDMul;
constexpr Code MC_DIV = Code::Div;
constexpr Code MC_DIVS = Code::Divs;
constexpr Code MC_UDIV = Code::UDiv;
constexpr Code MC_UDIVS = Code::UDivs;
constexpr Code MC_FDIV = Code::FDiv;
constexpr Code MC_DDIV = Code::DDiv;
constexpr Code MC_LDDIV = Code::LDDiv;
constexpr Code MC_MOD = Code::Mod;
constexpr Code MC_MODS = Code::Mods;
constexpr Code MC_UMOD = Code::UMod;
constexpr Code MC_UMODS = Code::UMods;
constexpr Code MC_AND = Code::And;
constexpr Code MC_ANDS = Code::Ands;
constexpr Code MC_OR = Code::Or;
constexpr Code MC_ORS = Code::Ors;
constexpr Code MC_XOR = Code::Xor;
constexpr Code MC_XORS = Code::Xors;
constexpr Code MC_LSH = Code::Lsh;
constexpr Code MC_LSHS = Code::Lshs;
constexpr Code MC_RSH = Code::Rsh;
constexpr Code MC_RSHS = Code::Rshs;
constexpr Code MC_URSH = Code::URsh;
constexpr Code MC_URSHS = Code::URshs;
constexpr Code MC_EQ = Code::Eq;
constexpr Code MC_EQS = Code::Eqs;
constexpr Code MC_FEQ = Code::FEq;
constexpr Code MC_DEQ = Code::DEq;
constexpr Code MC_LDEQ = Code::LDEq;
constexpr Code MC_NE = Code::Ne;
constexpr Code MC_NES = Code::Nes;
constexpr Code MC_FNE = Code::FNe;
constexpr Code MC_DNE = Code::DNe;
constexpr Code MC_LDNE = Code::LDNe;
constexpr Code MC_LT = Code::Lt;
constexpr Code MC_LTS = Code::Lts;
constexpr Code MC_ULT = Code::ULt;
constexpr Code MC_ULTS = Code::ULts;
constexpr Code MC_FLT = Code::FLt;
constexpr Code MC_DLT = Code::DLt;
constexpr Code MC_LDLT = Code::LDLt;
constexpr Code MC_LE = Code::Le;
constexpr Code MC_LES = Code::Les;
constexpr Code MC_ULE = Code::ULe;
constexpr Code MC_ULES = Code::ULes;
constexpr Code MC_FLE = Code::FLe;
constexpr Code MC_DLE = Code::DLe;
constexpr Code MC_LDLE = Code::LDLe;
constexpr Code MC_GT = Code::Gt;
constexpr Code MC_GTS = Code::Gts;
constexpr Code MC_UGT = Code::UGt;
constexpr Code MC_UGTS = Code::UGts;
constexpr Code MC_FGT = Code::FGt;
constexpr Code MC_DGT = Code::DGt;
constexpr Code MC_LDGT = Code::LDGt;
constexpr Code MC_GE = Code::Ge;
constexpr Code MC_GES = Code::Ges;
constexpr Code MC_UGE = Code::UGe;
constexpr Code MC_UGES = Code::UGes;
constexpr Code MC_FGE = Code::FGe;
constexpr Code MC_DGE = Code::DGe;
constexpr Code MC_LDGE = Code::LDGe;
constexpr Code MC_ADDO = Code::Addo;
constexpr Code MC_ADDOS = Code::Addos;
constexpr Code MC_SUBO = Code::Subo;
constexpr Code MC_SUBOS = Code::Subos;
constexpr Code MC_MULO = Code::Mulo;
constexpr Code MC_MULOS = Code::Mulos;
constexpr Code MC_UMULO = Code::UMulo;
constexpr Code MC_UMULOS = Code::UMulos;
constexpr Code MC_JMP = Code::Jmp;
constexpr Code MC_BT = Code::Bt;
constexpr Code MC_BTS = Code::Bts;
constexpr Code MC_BF = Code::Bf;
constexpr Code MC_BFS = Code::Bfs;
constexpr Code MC_BEQ = Code::Beq;
constexpr Code MC_BEQS = Code::Beqs;
constexpr Code MC_FBEQ = Code::FBeq;
constexpr Code MC_DBEQ = Code::DBeq;
constexpr Code MC_LDBEQ = Code::LDBeq;
constexpr Code MC_BNE = Code::Bne;
constexpr Code MC_BNES = Code::Bnes;
constexpr Code MC_FBNE = Code::FBne;
constexpr Code MC_DBNE = Code::DBne;
constexpr Code MC_LDBNE = Code::LDBne;
constexpr Code MC_BLT = Code::Blt;
constexpr Code MC_BLTS = Code::Blts;
constexpr Code MC_UBLT = Code::UBlt;
constexpr Code MC_UBLTS = Code::UBlts;
constexpr Code MC_FBLT = Code::FBlt;
constexpr Code MC_DBLT = Code::DBlt;
constexpr Code MC_LDBLT = Code::LDBlt;
constexpr Code MC_BLE = Code::Ble;
constexpr Code MC_BLES = Code::Bles;
constexpr Code MC_UBLE = Code::UBle;
constexpr Code MC_UBLES = Code::UBles;
constexpr Code MC_FBLE = Code::FBle;
constexpr Code MC_DBLE = Code::DBle;
constexpr Code MC_LDBLE = Code::LDBle;
constexpr Code MC_BGT = Code::Bgt;
constexpr Code MC_BGTS = Code::Bgts;
constexpr Code MC_UBGT = Code::UBgt;
constexpr Code MC_UBGTS = Code::UBgts;
constexpr Code MC_FBGT = Code::FBgt;
constexpr Code MC_DBGT = Code::DBgt;
constexpr Code MC_LDBGT = Code::LDBgt;
constexpr Code MC_BGE = Code::Bge;
constexpr Code MC_BGES = Code::Bges;
constexpr Code MC_UBGE = Code::UBge;
constexpr Code MC_UBGES = Code::UBges;
constexpr Code MC_FBGE = Code::FBge;
constexpr Code MC_DBGE = Code::DBge;
constexpr Code MC_LDBGE = Code::LDBge;
constexpr Code MC_BO = Code::Bo;
constexpr Code MC_UBO = Code::UBo;
constexpr Code MC_BNO = Code::Bno;
constexpr Code MC_UBNO = Code::UBno;
constexpr Code MC_LADDR = Code::LAddr;
constexpr Code MC_JMPI = Code::JmpIndirect;
constexpr Code MC_CALL = Code::Call;
constexpr Code MC_INLINE = Code::Inline;
constexpr Code MC_JCALL = Code::JCall;
constexpr Code MC_SWITCH = Code::Switch;
constexpr Code MC_RET = Code::Ret;
constexpr Code MC_JRET = Code::JRet;
constexpr Code MC_ALLOCA = Code::Alloca;
constexpr Code MC_BSTART = Code::BStart;
constexpr Code MC_BEND = Code::BEnd;
constexpr Code MC_VA_ARG = Code::VaArg;
constexpr Code MC_VA_BLOCK_ARG = Code::VaBlockArg;
constexpr Code MC_VA_START = Code::VaStart;
constexpr Code MC_VA_END = Code::VaEnd;
constexpr Code MC_LABEL = Code::Label;
constexpr Code MC_PRSET = Code::PrSet;
constexpr Code MC_PRBEQ = Code::PrBeq;
constexpr Code MC_PRBNE = Code::PrBne;
constexpr Code MC_UNSPEC = Code::Unspec;
constexpr Code MC_USE = Code::Use;
constexpr Code MC_PHI = Code::Phi;
constexpr Code MC_INVALID_INSN = Code::Invalid;
constexpr Code MC_INSN_BOUND = Code::Bound;
#define MIR_MOV MC_MOV
#define MIR_FMOV MC_FMOV
#define MIR_DMOV MC_DMOV
#define MIR_LDMOV MC_LDMOV
#define MIR_EXT8 MC_EXT8
#define MIR_EXT16 MC_EXT16
#define MIR_EXT32 MC_EXT32
#define MIR_UEXT8 MC_UEXT8
#define MIR_UEXT16 MC_UEXT16
#define MIR_UEXT32 MC_UEXT32
#define MIR_I2F MC_I2F
#define MIR_I2D MC_I2D
#define MIR_I2LD MC_I2LD
#define MIR_UI2F MC_UI2F
#define MIR_UI2D MC_UI2D
#define MIR_UI2LD MC_UI2LD
#define MIR_F2I MC_F2I
#define MIR_D2I MC_D2I
#define MIR_LD2I MC_LD2I
#define MIR_F2D MC_F2D
#define MIR_F2LD MC_F2LD
#define MIR_D2F MC_D2F
#define MIR_D2LD MC_D2LD
#define MIR_LD2F MC_LD2F
#define MIR_LD2D MC_LD2D
#define MIR_NEG MC_NEG
#define MIR_NEGS MC_NEGS
#define MIR_FNEG MC_FNEG
#define MIR_DNEG MC_DNEG
#define MIR_LDNEG MC_LDNEG
#define MIR_ADDR MC_ADDR
#define MIR_ADDR8 MC_ADDR8
#define MIR_ADDR16 MC_ADDR16
#define MIR_ADDR32 MC_ADDR32
#define MIR_ADD MC_ADD
#define MIR_ADDS MC_ADDS
#define MIR_FADD MC_FADD
#define MIR_DADD MC_DADD
#define MIR_LDADD MC_LDADD
#define MIR_SUB MC_SUB
#define MIR_SUBS MC_SUBS
#define MIR_FSUB MC_FSUB
#define MIR_DSUB MC_DSUB
#define MIR_LDSUB MC_LDSUB
#define MIR_MUL MC_MUL
#define MIR_MULS MC_MULS
#define MIR_FMUL MC_FMUL
#define MIR_DMUL MC_DMUL
#define MIR_LDMUL MC_LDMUL
#define MIR_DIV MC_DIV
#define MIR_DIVS MC_DIVS
#define MIR_UDIV MC_UDIV
#define MIR_UDIVS MC_UDIVS
#define MIR_FDIV MC_FDIV
#define MIR_DDIV MC_DDIV
#define MIR_LDDIV MC_LDDIV
#define MIR_MOD MC_MOD
#define MIR_MODS MC_MODS
#define MIR_UMOD MC_UMOD
#define MIR_UMODS MC_UMODS
#define MIR_AND MC_AND
#define MIR_ANDS MC_ANDS
#define MIR_OR MC_OR
#define MIR_ORS MC_ORS
#define MIR_XOR MC_XOR
#define MIR_XORS MC_XORS
#define MIR_LSH MC_LSH
#define MIR_LSHS MC_LSHS
#define MIR_RSH MC_RSH
#define MIR_RSHS MC_RSHS
#define MIR_URSH MC_URSH
#define MIR_URSHS MC_URSHS
#define MIR_EQ MC_EQ
#define MIR_EQS MC_EQS
#define MIR_FEQ MC_FEQ
#define MIR_DEQ MC_DEQ
#define MIR_LDEQ MC_LDEQ
#define MIR_NE MC_NE
#define MIR_NES MC_NES
#define MIR_FNE MC_FNE
#define MIR_DNE MC_DNE
#define MIR_LDNE MC_LDNE
#define MIR_LT MC_LT
#define MIR_LTS MC_LTS
#define MIR_ULT MC_ULT
#define MIR_ULTS MC_ULTS
#define MIR_FLT MC_FLT
#define MIR_DLT MC_DLT
#define MIR_LDLT MC_LDLT
#define MIR_LE MC_LE
#define MIR_LES MC_LES
#define MIR_ULE MC_ULE
#define MIR_ULES MC_ULES
#define MIR_FLE MC_FLE
#define MIR_DLE MC_DLE
#define MIR_LDLE MC_LDLE
#define MIR_GT MC_GT
#define MIR_GTS MC_GTS
#define MIR_UGT MC_UGT
#define MIR_UGTS MC_UGTS
#define MIR_FGT MC_FGT
#define MIR_DGT MC_DGT
#define MIR_LDGT MC_LDGT
#define MIR_GE MC_GE
#define MIR_GES MC_GES
#define MIR_UGE MC_UGE
#define MIR_UGES MC_UGES
#define MIR_FGE MC_FGE
#define MIR_DGE MC_DGE
#define MIR_LDGE MC_LDGE
#define MIR_ADDO MC_ADDO
#define MIR_ADDOS MC_ADDOS
#define MIR_SUBO MC_SUBO
#define MIR_SUBOS MC_SUBOS
#define MIR_MULO MC_MULO
#define MIR_MULOS MC_MULOS
#define MIR_UMULO MC_UMULO
#define MIR_UMULOS MC_UMULOS
#define MIR_JMP MC_JMP
#define MIR_BT MC_BT
#define MIR_BTS MC_BTS
#define MIR_BF MC_BF
#define MIR_BFS MC_BFS
#define MIR_BEQ MC_BEQ
#define MIR_BEQS MC_BEQS
#define MIR_FBEQ MC_FBEQ
#define MIR_DBEQ MC_DBEQ
#define MIR_LDBEQ MC_LDBEQ
#define MIR_BNE MC_BNE
#define MIR_BNES MC_BNES
#define MIR_FBNE MC_FBNE
#define MIR_DBNE MC_DBNE
#define MIR_LDBNE MC_LDBNE
#define MIR_BLT MC_BLT
#define MIR_BLTS MC_BLTS
#define MIR_UBLT MC_UBLT
#define MIR_UBLTS MC_UBLTS
#define MIR_FBLT MC_FBLT
#define MIR_DBLT MC_DBLT
#define MIR_LDBLT MC_LDBLT
#define MIR_BLE MC_BLE
#define MIR_BLES MC_BLES
#define MIR_UBLE MC_UBLE
#define MIR_UBLES MC_UBLES
#define MIR_FBLE MC_FBLE
#define MIR_DBLE MC_DBLE
#define MIR_LDBLE MC_LDBLE
#define MIR_BGT MC_BGT
#define MIR_BGTS MC_BGTS
#define MIR_UBGT MC_UBGT
#define MIR_UBGTS MC_UBGTS
#define MIR_FBGT MC_FBGT
#define MIR_DBGT MC_DBGT
#define MIR_LDBGT MC_LDBGT
#define MIR_BGE MC_BGE
#define MIR_BGES MC_BGES
#define MIR_UBGE MC_UBGE
#define MIR_UBGES MC_UBGES
#define MIR_FBGE MC_FBGE
#define MIR_DBGE MC_DBGE
#define MIR_LDBGE MC_LDBGE
#define MIR_BO MC_BO
#define MIR_UBO MC_UBO
#define MIR_BNO MC_BNO
#define MIR_UBNO MC_UBNO
#define MIR_LADDR MC_LADDR
#define MIR_JMPI MC_JMPI
#define MIR_CALL MC_CALL
#define MIR_INLINE MC_INLINE
#define MIR_JCALL MC_JCALL
#define MIR_SWITCH MC_SWITCH
#define MIR_RET MC_RET
#define MIR_JRET MC_JRET
#define MIR_ALLOCA MC_ALLOCA
#define MIR_BSTART MC_BSTART
#define MIR_BEND MC_BEND
#define MIR_VA_ARG MC_VA_ARG
#define MIR_VA_BLOCK_ARG MC_VA_BLOCK_ARG
#define MIR_VA_START MC_VA_START
#define MIR_VA_END MC_VA_END
#define MIR_LABEL MC_LABEL
#define MIR_PRSET MC_PRSET
#define MIR_PRBEQ MC_PRBEQ
#define MIR_PRBNE MC_PRBNE
#define MIR_UNSPEC MC_UNSPEC
#define MIR_USE MC_USE
#define MIR_PHI MC_PHI
#define MIR_INVALID_INSN MC_INVALID_INSN
#define MIR_INSN_BOUND MC_INSN_BOUND
#define MOVDQA_CODE 0
