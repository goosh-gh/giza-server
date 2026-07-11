#!/bin/bash
# foreground_giza_server.sh
#
# 判明した事実: Driver::GS::_launch_server (auto起動経路) は
# fork()した子プロセスのSTDOUT/STDERRを/dev/nullにリダイレクトしてから
# exec()する。これは元々「サーバーは常駐するのでターミナルを汚さない」
# という設計意図(pgxwin_server後継としては正しい)だが、結果として
# giza_server側のfprintf(stderr,...)診断ログは、Driver::GS経由で
# 自動起動された場合は一切見えない。
#
# 今までの一連の診断("[DIAG] drawRect: called"が出ない等)が出なかった
# 根本原因はこれ。GIZA_SERVER環境変数を指定しても、_ensure_serverが
# auto起動した場合は同じ問題が起きる。
#
# このスクリプトは、giza_serverを「手動で、確実にこのターミナルの
# 子プロセスとしてフォアグラウンド起動」し、Driver::GS側には
# start=>'connect'(起動しない、居なければdie)で繋がせる構成を案内する。

echo "=== 1. 既存のgiza_serverプロセスを全部止める ==="
pkill -f giza_server
sleep 1
ps aux | grep -i giza_server | grep -v grep
echo "(↑何も出なければOK)"

echo ""
echo "=== 2. ソケットの残骸を削除 ==="
rm -f "/tmp/giza_server_$(id -u).sock"

echo ""
echo "=== 3. 次のステップ ==="
echo "別ターミナルを開いて、以下を実行してください"
echo "(このターミナル自体で実行すると、フォアグラウンドなのでブロックされます):"
echo ""
echo "  cd ~/src/giza-server"
echo "  ./giza_server"
echo ""
echo "これで起動直後に"
echo "  giza_server (cocoa): build <日時>"
echo "という行がそのターミナルに出るはずです(出なければビルド刻印自体の"
echo "問題なので、また別途調査が必要)。"
echo ""
echo "そのターミナルを開いたままにしておき、元のターミナル(このスクリプトを"
echo "実行した方)で以下を実行してテストスクリプトを動かしてください:"
echo ""
echo "  cd ~/src/giza-server"
echo "  perl -I./lib/ -I/Users/goosh/src/PDL_Graphics_Cairo/lib/ ./download/test_gs3d_electrode.pl"
echo ""
echo "GIZA_SERVER環境変数は不要です(start=>'auto'のままで、すでに手動起動"
echo "済みのgiza_serverに'居れば共存'で繋がります)。"
echo ""
echo "[DIAG]ログは、giza_serverを起動した方のターミナル(別ターミナル)に"
echo "出ます。test_gs3d_electrode.plを実行した方のターミナルには"
echo "Perl側の[DIAG] 3D_INPUT received...だけが出ます。"
