/*------------------------------------------------------------*/
/* Русифицированная замена source/tvision/tvtext2.cpp         */
/* библиотеки Turbo Vision. Функции перенесены без изменений, */
/* переведены строки стандартных диалогов и окон.            */
/*------------------------------------------------------------*/
/*
 *      Turbo Vision - Version 2.0
 *
 *      Copyright (c) 1994 by Borland International
 *      All Rights Reserved.
 *
 */

#define Uses_TEditWindow
#define Uses_TFileList
#define Uses_TProgram
#define Uses_MsgBox
#define Uses_TChDirDialog
#define Uses_TFileDialog
#define Uses_TFileInfoPane
#define Uses_TSystemError
#define Uses_TDeskTop
#define Uses_TPXPictureValidator
#define Uses_TFilterValidator
#define Uses_TRangeValidator
#define Uses_TStringLookupValidator
#define Uses_TListViewer
#include <tvision/tv.h>
#include <tvision/help.h>

#if !defined( __CTYPE_H )
#include <ctype.h>
#endif  // __CTYPE_H

static const char altCodes1[] =
    "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
static const char altCodes2[] = "1234567890-=";

#pragma warn -rng

char getAltChar(ushort keyCode)
{
    if ((keyCode & 0xff) == 0)
        {
        ushort tmp = (keyCode >> 8);

        if( tmp == 2 )
            return '\xF0';      // special case to handle alt-Space

        else if( tmp >= 0x10 && tmp <= 0x32 )
            return altCodes1[tmp-0x10];     // alt-letter

        else if( tmp >= 0x78 && tmp <= 0x83 )
            return altCodes2[tmp - 0x78];   // alt-number

        }
    return 0;
}

ushort getAltCode(char c)
{
    if( c == 0 )
        return 0;

    c = toupper(c);

    if( c == '\xF0' )
        return 0x200;       // special case to handle alt-Space

    size_t i;
    for( i = 0; i < sizeof( altCodes1 ); i++)
       if( altCodes1[i] == c )
        return (i+0x10) << 8;

    for( i = 0; i < sizeof( altCodes2); i++)
        if (altCodes2[i] == c)
            return (i+0x78) << 8;

    return 0;
}

inline uchar lo(ushort w) { return w & 0xff; }
inline uchar hi(ushort w) { return w >> 8; }

char getCtrlChar(ushort keyCode)
{
    if ( (lo(keyCode)!= 0) && (lo(keyCode) <= ('Z'-'A'+1)))
        return lo(keyCode) + 'A' - 1;
    else
        return 0;
}

ushort getCtrlCode(uchar ch)
{
	return getAltCode(ch)|(((('a'<=ch)&&(ch<='z'))?(ch&~0x20):ch)-'A'+1);

}


#pragma warn .rng


const char * _NEAR TPXPictureValidator::errorMsg = "Ошибка в формате шаблона.\n %s";
const char * _NEAR TFilterValidator::errorMsg = "Недопустимый символ";
const char * _NEAR TRangeValidator::errorMsg = "Значение вне диапазона от %ld до %ld";
const char * _NEAR TStringLookupValidator::errorMsg = "Значение отсутствует в списке допустимых";

const char * _NEAR TRangeValidator::validUnsignedChars = "+0123456789";
const char * _NEAR TRangeValidator::validSignedChars = "+-0123456789";

const char * _NEAR TListViewer::emptyText = "<пусто>";

const char * _NEAR THelpWindow::helpWinTitle = "Справка";
const char * _NEAR THelpFile::invalidContext =
    "\n Справка для этого раздела отсутствует.";

const char * _NEAR TEditWindow::clipboardTitle = "Буфер обмена";
const char * _NEAR TEditWindow::untitled = "Без имени";

const char * _NEAR TFileList::tooManyFiles = "Слишком много файлов.";

const char * _NEAR TProgram::exitText = "~Alt-X~ Выход";

const char * _NEAR MsgBoxText::yesText = "~Д~а";
const char * _NEAR MsgBoxText::noText = "~Н~ет";
const char * _NEAR MsgBoxText::okText = "O~K~";
const char * _NEAR MsgBoxText::cancelText = "Отмена";
const char * _NEAR MsgBoxText::warningText = "Предупреждение";
const char * _NEAR MsgBoxText::errorText = "Ошибка";
const char * _NEAR MsgBoxText::informationText = "Информация";
const char * _NEAR MsgBoxText::confirmText = "Подтверждение";

const char * _NEAR TChDirDialog::changeDirTitle = "Выбор директории";
const char * _NEAR TChDirDialog::dirNameText = "~И~мя директории";
const char * _NEAR TChDirDialog::dirTreeText = "~Д~иректории";
const char * _NEAR TChDirDialog::okText = "O~K~";
const char * _NEAR TChDirDialog::chdirText = "~П~ерейти";
const char * _NEAR TChDirDialog::revertText = "~В~ернуть";
const char * _NEAR TChDirDialog::helpText = "Справка";
const char * _NEAR TChDirDialog::drivesText = "Диски";
const char * _NEAR TChDirDialog::invalidText = "Недопустимая директория";

const char * _NEAR TFileDialog::filesText = "~Ф~айлы";
const char * _NEAR TFileDialog::openText = "~О~ткрыть";
const char * _NEAR TFileDialog::okText = "O~K~";
const char * _NEAR TFileDialog::replaceText = "~З~аменить";
const char * _NEAR TFileDialog::clearText = "О~ч~истить";
const char * _NEAR TFileDialog::cancelText = "Отмена";
const char * _NEAR TFileDialog::helpText = "~С~правка";
const char * _NEAR TFileDialog::invalidDriveText = "Недопустимый диск или директория";
const char * _NEAR TFileDialog::invalidFileText = "Недопустимое имя файла";

const char * _NEAR TFileInfoPane::pmText = "p";
const char * _NEAR TFileInfoPane::amText = "a";
const char * const _NEAR TFileInfoPane::months[] =
    {
    "","янв","фев","мар","апр","мая","июн",
    "июл","авг","сен","окт","ноя","дек"
    };

const char _NEAR TDeskTop::defaultBkgrnd = '\xB0';

#if !defined( __FLAT__ )
const char * const _NEAR TSystemError::errorString[] =
{
    "Disk in drive %c is write protected",          // 0
    "Unknown unit %c",                              // 1 - NEW
    "Disk is not ready in drive %c",                // 2
    "Critical error (unknown command) on drive %c", // 3 - MODIFIED
    "Data integrity error on drive %c",             // 4
    "Critical error (bad request) on drive %c",     // 5 - NEW/MODIFIED
    "Seek error on drive %c",                       // 6
    "Unknown media type in drive %c",               // 7
    "Sector not found on drive %c",                 // 8
    "Printer out of paper",                         // 9
    "Write fault on drive %c",                      // A
    "Read fault on drive %c",                       // B
    "General failure on drive %c",                  // C
    "Sharing violation on drive %c",                // D
    "Lock violation on drive %c",                   // E
    "Disk change invalid on drive %c",              // F
    "FCB unavailable",                              //10
    "Sharing buffer overflow",                      //11
    "Code page mismatch",                           //12
    "Out of input",                                 //13
    "Insufficient disk space on drive %c",          //14
    "Insert diskette in drive %c"                   //15
};

const char * _NEAR TSystemError::sRetryOrCancel = "~Enter~ Retry  ~Esc~ Cancel";
#endif
