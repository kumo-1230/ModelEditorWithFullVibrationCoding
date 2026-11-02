#pragma once
// 未処理の例外 (クラッシュ) を捕まえて %APPDATA%\ModelEditor\crash_*.dmp にミニダンプを保存する
namespace CrashHandler {
void Install(bool showMessageBox);
}
