// Расшифровка 7z (метод 7zAES: AES-256-CBC, ключ из пароля через SHA-256)
// поверх распаковщика LZMA SDK. Функция SzAr_DecodeFolder из 7zDec.c
// заменена своей: зашифрованный поток расшифровывается в память, после чего
// остальная цепочка методов распаковывается штатным кодом SDK.
#ifndef CABINE_7ZAES_H
#define CABINE_7ZAES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Архив зашифрован, а пароль не задан.
#define SZ_ERROR_NO_PASSWORD 0x7A01

// Пароль в UTF-16LE; nullptr — пароля нет. Действует до следующего вызова.
void Sz7zAes_SetPassword(const unsigned short *utf16, size_t chars);
// Встречались ли зашифрованные данные после последнего сброса.
int Sz7zAes_Used(void);
void Sz7zAes_ResetUsed(void);

#ifdef __cplusplus
}
#endif

#endif
