# VL53L9 on NUCLEO-N657X0-Q (omarchy / Arch Linux)

STEVAL-VL53L9 + NUCLEO-N657X0-Q で ST 公式サンプル (STSW-IMG054, `vl53l9-nucleo-n657-fmw-1.1.1`) を
**STM32CubeIDE / CubeProgrammer / ST アカウントなし** で動かすためのリポジトリ。

- ビルド: `arm-none-eabi-gcc` + `Makefile`(`.cproject` の設定を移植)
- 書き込み: 内蔵 STLINK-V3EC + OpenOCD で **SRAM に直接ロード**(開発ブート。署名・外部フラッシュ不要)
- 出力: LPUART1 → ST-LINK VCP (`/dev/ttyACM*`)、**921600 baud**

> ST パッケージのうち SLA0111 ライセンスの `app/` と `lib/` は再配布できないため含まれていない。
> [STSW-IMG054](https://www.st.com/en/embedded-software/stsw-img054.html) を st.com から取得して:
>
> ```sh
> scripts/import-st-package.sh ~/Downloads/vl53l9-nucleo-n657-1.0.0.zip
> ```
>
> 詳細は [vendor/README.md](vendor/README.md)。`CAD/` には STEVAL-VL53L9 用ケースがある。

## 1. 初回セットアップ (omarchy)

```sh
git clone https://github.com/zhixuan2333/VL53L9-example && cd VL53L9-example
scripts/import-st-package.sh ~/Downloads/vl53l9-nucleo-n657-1.0.0.zip   # ST の zip から app/ lib/
./scripts/setup-omarchy.sh   # toolchain, openocd-git (AUR), udev, uucp group
# ログアウト/ログインし直す
```

## 2. ハードウェア

- STEVAL-VL53L9 を NUCLEO の **カメラコネクタ** に接続(デフォルト設定 `CONFIG_HW_STEVAL_CAM`)
- **JP2 (BOOT1) = 2-3**(開発ブート)、JP1 (BOOT0) = 1-2
- ST-LINK 側 USB-C を PC に接続

## 3. ビルド & 実行

```sh
make                      # APP=ranging_i3c (デフォルト)
make run                  # SRAM にロードして実行 (OpenOCD)
make run-probe-rs         # 同じことを probe-rs で
make monitor              # シリアル (picocom, 終了は C-a C-x)
```

| APP | 内容 |
|---|---|
| `ranging_i3c` | I3C でデータ取得(まずこれ) |
| `ranging_csi` | MIPI CSI-2 でデータ取得 |
| `ranging_multi_i3c` | センサ 2 台以上が必要 |
| `explorer` | 全パラメータを Web UI から変更（下記, `apps/explorer/`） |

深度フレームを ASCII で表示:

```sh
make clean && make EXTRA_DEFS=-DCONF_PRINT_FRAME=1 && make run
```

## 4. Explorer (Web UI で全パラメータを試す)

```sh
make APP=explorer run-probe-rs   # ファームを SRAM にロード（起動時は UART に ASCII 深度マップ）
make web                         # 別ターミナル: http://localhost:8000 を Chromium で開いて「接続」
```

- センサー設定（sync / binning / exposure / frame rate / context / DSS / DSS index / power / usecase プリセット）と
  ST transform の全コントロール（ファームが実行時に一覧を送る）を変更すると、その場でパイプラインを再構築する。
- Profile: ST の 4 usecase に加え、現在の設定（センサー・ストリーム・transform）を名前を付けて保存/適用できる。
  保存先はブラウザ (localStorage)。「書き出し」「読み込み」で JSON ファイルとしてやり取りできる。
- 出力ストリーム（depth / amplitude / ambient / confidence / reflectance / status）を選んでヒートマップ表示、
  ホバーで画素値、ASCII 表示。921600 baud では depth のみ 54×42 で約 10 fps が上限（2 M 以上は実験的）。
- picocom で直接使う場合: `out ascii` / `out text` / `get` / `set binning=4 exposure=8` / `status`
  （プロトコルは `apps/explorer/vl53l9_app.c` 冒頭）。Web UI と picocom は同時にポートを開けない。
- ハード無しで UI だけ試す: `http://localhost:8000/#demo`

`src/irq_unmask.c` は probe-rs のバグ (#4390: RAM 実行時に割り込みが無効のまま) の回避策。

SRAM 実行なので、リセット/電源断で消える。
フラッシュに焼く場合は ST の CubeProgrammer(署名ツール)が必要 → `vendor/.../README.md` 参照。
