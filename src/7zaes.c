// Расшифровка 7z (7zAES) для распаковщика LZMA SDK. См. 7zaes.h.
//
// 7zDec.c подключается целиком с переименованной SzAr_DecodeFolder: так
// доступна его статическая SzFolder_Decode2, а исходники SDK не меняются.
#define SzAr_DecodeFolder SzAr_DecodeFolder_Plain
#include "7zDec.c"
#undef SzAr_DecodeFolder

#include "Aes.h"
#include "Sha256.h"

#include <stdlib.h>

#include "7zaes.h"

#define k_AES 0x6F10701
// Как в 7-Zip: больше 2^24 циклов SHA-256 не принимается.
#define kNumCyclesPowerMax 24

static Byte *g_password;
static size_t g_passwordSize;   // в байтах
static BoolInt g_havePassword;
static int g_used;

// Последний вычисленный ключ: у всех блоков архива обычно одни соль и число циклов.
static Byte g_keyCache[32];
static Byte g_keySalt[16];
static unsigned g_keySaltSize;
static unsigned g_keyPower;
static BoolInt g_keyValid;

void Sz7zAes_SetPassword(const unsigned short *utf16, size_t chars)
{
    size_t i;
    free(g_password);
    g_password = NULL;
    g_passwordSize = 0;
    g_havePassword = utf16 != NULL;
    g_keyValid = False;
    if (!utf16 || chars == 0)
        return;
    g_password = (Byte *)malloc(chars * 2);
    if (!g_password)
    {
        g_havePassword = False;
        return;
    }
    for (i = 0; i < chars; i++)
    {
        g_password[i * 2] = (Byte)(utf16[i] & 0xFF);
        g_password[i * 2 + 1] = (Byte)(utf16[i] >> 8);
    }
    g_passwordSize = chars * 2;
}

int Sz7zAes_Used(void) { return g_used; }
void Sz7zAes_ResetUsed(void) { g_used = 0; }

static void InitCrypto(void)
{
    static BoolInt ready;
    if (!ready)
    {
        AesGenTables();
        Sha256Prepare();
        ready = True;
    }
}

// Ключ: SHA-256 от 2^power повторов (соль, пароль, 8-байтовый счётчик);
// при power == 0x3F — соль и пароль как есть, дополненные нулями.
static void DeriveKey(const Byte *salt, unsigned saltSize, unsigned power, Byte *key)
{
    if (g_keyValid && g_keyPower == power && g_keySaltSize == saltSize &&
        memcmp(g_keySalt, salt, saltSize) == 0)
    {
        memcpy(key, g_keyCache, 32);
        return;
    }
    if (power == 0x3F)
    {
        size_t pos = 0, i;
        memset(key, 0, 32);
        for (i = 0; i < saltSize && pos < 32; i++)
            key[pos++] = salt[i];
        for (i = 0; i < g_passwordSize && pos < 32; i++)
            key[pos++] = g_password[i];
    }
    else
    {
        CSha256 sha;
        Byte counter[8] = {0};
        UInt64 rounds = (UInt64)1 << power, r;
        Sha256_Init(&sha);
        for (r = 0; r < rounds; r++)
        {
            unsigned k;
            Sha256_Update(&sha, salt, saltSize);
            Sha256_Update(&sha, g_password, g_passwordSize);
            Sha256_Update(&sha, counter, 8);
            for (k = 0; k < 8 && ++counter[k] == 0; k++)
            {
            }
        }
        Sha256_Final(&sha, key);
    }
    memcpy(g_keyCache, key, 32);
    memcpy(g_keySalt, salt, saltSize);
    g_keySaltSize = saltSize;
    g_keyPower = power;
    g_keyValid = True;
}

// Свойства метода: число циклов, соль и вектор инициализации.
static SRes Decrypt(const Byte *props, unsigned propsSize, Byte *data, size_t size)
{
    unsigned power, saltSize = 0, ivSize = 0;
    Byte salt[16], iv[AES_BLOCK_SIZE], key[32];
    UInt32 aesBuf[AES_NUM_IVMRK_WORDS + 4];
    UInt32 *aes = aesBuf;

    if (propsSize < 1)
        return SZ_ERROR_UNSUPPORTED;
    power = props[0] & 0x3F;
    memset(iv, 0, sizeof(iv));
    if ((props[0] & 0xC0) != 0)
    {
        if (propsSize < 2)
            return SZ_ERROR_UNSUPPORTED;
        saltSize = ((props[0] >> 7) & 1) + (props[1] >> 4);
        ivSize = ((props[0] >> 6) & 1) + (props[1] & 0x0F);
        if (propsSize != 2 + saltSize + ivSize)
            return SZ_ERROR_UNSUPPORTED;
        memcpy(salt, props + 2, saltSize);
        memcpy(iv, props + 2 + saltSize, ivSize);
    }
    else if (propsSize != 1)
        return SZ_ERROR_UNSUPPORTED;
    if (power > kNumCyclesPowerMax && power != 0x3F)
        return SZ_ERROR_UNSUPPORTED;
    if (size % AES_BLOCK_SIZE != 0)
        return SZ_ERROR_DATA;

    InitCrypto();
    DeriveKey(salt, saltSize, power, key);
    // Таблицы AES с аппаратным ускорением требуют выравнивания по 16 байтам.
    while (((size_t)aes & 15) != 0)
        aes++;
    Aes_SetKey_Dec(aes + 4, key, 32);
    AesCbc_Init(aes, iv);
    g_AesCbc_Decode(aes, data, size / AES_BLOCK_SIZE);
    memset(key, 0, sizeof(key));
    return SZ_OK;
}

// Поток из памяти для SzFolder_Decode2.
typedef struct
{
    ILookInStream vt;
    const Byte *data;
    size_t size;
    size_t pos;
} CMemLookIn;

static SRes MemLook(ILookInStreamPtr pp, const void **buf, size_t *size)
{
    CMemLookIn *p = (CMemLookIn *)(void *)pp;
    size_t rem = p->size - p->pos;
    if (*size > rem)
        *size = rem;
    *buf = p->data + p->pos;
    return SZ_OK;
}

static SRes MemSkip(ILookInStreamPtr pp, size_t offset)
{
    CMemLookIn *p = (CMemLookIn *)(void *)pp;
    if (offset > p->size - p->pos)
        return SZ_ERROR_INPUT_EOF;
    p->pos += offset;
    return SZ_OK;
}

static SRes MemRead(ILookInStreamPtr pp, void *buf, size_t *size)
{
    const void *src;
    RINOK(MemLook(pp, &src, size))
    memcpy(buf, src, *size);
    return MemSkip(pp, *size);
}

static SRes MemSeek(ILookInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    CMemLookIn *p = (CMemLookIn *)(void *)pp;
    Int64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? (Int64)p->pos : (Int64)p->size;
    Int64 to = base + *pos;
    if (to < 0 || (UInt64)to > p->size)
        return SZ_ERROR_PARAM;
    p->pos = (size_t)to;
    *pos = to;
    return SZ_OK;
}

SRes SzAr_DecodeFolder(const CSzAr *p, UInt32 folderIndex,
    ILookInStreamPtr inStream, UInt64 startPos,
    Byte *outBuffer, size_t outSize,
    ISzAllocPtr allocMain)
{
    CSzFolder folder, plain;
    CSzData sd;
    const Byte *data = p->CodersData + p->FoCodersOffsets[folderIndex];
    const UInt64 *unpackSizes = &p->CoderUnpackSizes[p->FoToCoderUnpackSizes[folderIndex]];
    const UInt64 *packPositions = p->PackPositions + p->FoStartPackStreamIndex[folderIndex];
    UInt64 plainSizes[SZ_NUM_CODERS_IN_FOLDER_MAX];
    UInt64 memPositions[2];
    UInt32 a, i, nextCoder = 0;
    UInt64 packSize, aesSize;
    Byte *buf;
    SRes res;

    sd.Data = data;
    sd.Size = p->FoCodersOffsets[(size_t)folderIndex + 1] - p->FoCodersOffsets[folderIndex];
    RINOK(SzGetNextFolderItem(&folder, &sd))

    for (a = 0; a < folder.NumCoders; a++)
        if (folder.Coders[a].MethodID == k_AES)
            break;
    if (a == folder.NumCoders)
        return SzAr_DecodeFolder_Plain(p, folderIndex, inStream, startPos, outBuffer, outSize, allocMain);

    g_used = 1;
    if (!g_havePassword)
        return SZ_ERROR_NO_PASSWORD;
    if (sd.Size != 0
        || folder.UnpackStream != p->FoToMainUnpackSizeIndex[folderIndex]
        || outSize != SzAr_GetFolderUnpackSize(p, folderIndex))
        return SZ_ERROR_FAIL;

    // Поддерживается шифрование единственного упакованного потока:
    // упакованные данные → AES → остальная цепочка (без BCJ2).
    for (i = 0; i < folder.NumCoders; i++)
        if (folder.Coders[i].NumStreams != 1)
            return SZ_ERROR_UNSUPPORTED;
    if (folder.NumPackStreams != 1 || folder.PackStreams[0] != a)
        return SZ_ERROR_UNSUPPORTED;

    packSize = packPositions[1] - packPositions[0];
    aesSize = unpackSizes[a];
    if (aesSize > packSize)
        return SZ_ERROR_DATA;
    if ((size_t)packSize != packSize)
        return SZ_ERROR_MEM;

    // Цепочка без AES: индексы coder'ов после него сдвигаются на один.
    plain.NumCoders = 0;
    plain.NumBonds = 0;
    plain.NumPackStreams = 1;
    plain.PackStreams[0] = 0;
    plain.UnpackStream = folder.UnpackStream > a ? folder.UnpackStream - 1 : folder.UnpackStream;
    for (i = 0; i < folder.NumCoders; i++)
        if (i != a)
        {
            plainSizes[plain.NumCoders] = unpackSizes[i];
            plain.Coders[plain.NumCoders++] = folder.Coders[i];
        }
    for (i = 0; i < folder.NumBonds; i++)
    {
        CSzBond b = folder.Bonds[i];
        if (b.OutIndex == a)
        {
            nextCoder = b.InIndex > a ? b.InIndex - 1 : b.InIndex;
            continue;
        }
        if (b.InIndex == a)
            return SZ_ERROR_UNSUPPORTED;
        if (b.InIndex > a)
            b.InIndex--;
        if (b.OutIndex > a)
            b.OutIndex--;
        plain.Bonds[plain.NumBonds++] = b;
    }
    if (plain.NumCoders > 0 && plain.NumBonds + 1 != folder.NumBonds)
        return SZ_ERROR_UNSUPPORTED;
    plain.PackStreams[0] = nextCoder;

    buf = (Byte *)ISzAlloc_Alloc(allocMain, (size_t)packSize);
    if (!buf && packSize != 0)
        return SZ_ERROR_MEM;
    res = LookInStream_SeekTo(inStream, startPos + packPositions[0]);
    if (res == SZ_OK)
        res = LookInStream_Read(inStream, buf, (size_t)packSize);
    if (res == SZ_OK)
        res = Decrypt(data + folder.Coders[a].PropsOffset, folder.Coders[a].PropsSize, buf, (size_t)packSize);

    if (res == SZ_OK)
    {
        if (plain.NumCoders == 0)
        {
            // Только шифрование, без сжатия.
            if (aesSize != outSize)
                res = SZ_ERROR_DATA;
            else
                memcpy(outBuffer, buf, outSize);
        }
        else
        {
            CMemLookIn mem;
            Byte *tempBuf[3] = {0, 0, 0};
            mem.vt.Look = MemLook;
            mem.vt.Skip = MemSkip;
            mem.vt.Read = MemRead;
            mem.vt.Seek = MemSeek;
            mem.data = buf;
            mem.size = (size_t)aesSize;
            mem.pos = 0;
            memPositions[0] = 0;
            memPositions[1] = aesSize;
            res = SzFolder_Decode2(&plain, data, plainSizes, memPositions, &mem.vt, 0,
                                   outBuffer, (SizeT)outSize, allocMain, tempBuf);
            for (i = 0; i < 3; i++)
                ISzAlloc_Free(allocMain, tempBuf[i]);
        }
    }
    ISzAlloc_Free(allocMain, buf);

    if (res == SZ_OK)
        if (SzBitWithVals_Check(&p->FolderCRCs, folderIndex))
            if (CrcCalc(outBuffer, outSize) != p->FolderCRCs.Vals[folderIndex])
                res = SZ_ERROR_CRC;
    return res;
}
