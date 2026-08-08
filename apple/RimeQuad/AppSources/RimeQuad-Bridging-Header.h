//
//  RimeQuad-Bridging-Header.h
//
//  Swift 端唯一看得到的 C 介面就是這一個標頭。
//  **不要**在這裡加 librime 自己的 rime_api.h —— 四端共用的邊界是 rime_shell.h，
//  繞過它就等於這一端自己長出一套 librime 用法，之後的契約修正不會傳到這裡來。
//
#import "rime_shell.h"
