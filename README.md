<div align="center">

# 🦆 GameDuck

### 키 하나 누르면, 게임 소리만 조용해진다.

**팀원 목소리는 그대로, 게임 소리만 즉시 낮추는 윈도우 덕커**

[![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?style=flat-square&logo=windows&logoColor=white)](https://www.microsoft.com/windows)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![Native](https://img.shields.io/badge/Native-Win32%20%7C%20No%20.NET-111111?style=flat-square)](https://learn.microsoft.com/windows/win32/)
[![Size](https://img.shields.io/badge/Size-937KB%20single%20exe-2EA043?style=flat-square)](https://github.com/micio169/GameDuck/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)](LICENSE)
[![Version](https://img.shields.io/badge/Version-v1.0.0-9E6BFF?style=flat-square)](https://github.com/micio169/GameDuck/releases/tag/v1.0.0)

[⬇️ GameDuck.exe 다운로드](https://github.com/micio169/GameDuck/releases) · [사용법](#-사용법) · [원리](#-어떻게-작동하나) · [직접 빌드하기](#-직접-빌드하기)

</div>

---

## ✨ 왜 GameDuck인가

|  |  |
|---|---|
| **⚡ 즉시** | 누르는 순간 게임 볼륨이 낮아집니다. 페이드 0ms가 기본값입니다. |
| **🎯 게임만** | 시스템 볼륨, 디스코드, 음악은 그대로. 게임 프로세스 볼륨만 조절합니다. |
| **⌨️ 아무 키나** | 키보드 아무 키, 마우스 좌/우/휠/X1/X2 — 여러 개 등록하면 아무거나 눌러도 됩니다. |
| **🪶 가볍다** | 네이티브 C++ 단일 exe. 이벤트 방식 훅이라 안 누르면 CPU를 거의 안 씁니다. |
| **🛡️ 안전하다** | 게임 주입 없음, 게임 메모리 읽기 없음. Windows 오디오 세션 API만 씁니다. |

## 🚀 빠른 시작

```
1. Releases에서 GameDuck.exe 다운로드
2. 더블클릭으로 실행 (설치 없음)
3. 마우스 X1 옆 버튼을 누르면 게임 볼륨 10%로 ↓
4. 버튼을 떼면 원래 볼륨으로 복원
```

> 설정은 exe 옆 `GameDuck.ini`에 자동 저장됩니다.

## 🎮 사용법

### 1️⃣ 트리거 등록
`[키 / 마우스 추가]` → 키보드 키나 마우스 버튼을 한 번 누릅니다.

- 키보드: `F1`~`F12`, `CapsLock`, `스페이스`, 숫자, `Ctrl`/`Alt`/`Shift` 조합까지 전부 가능
- 마우스: 왼쪽 / 오른쪽 / 휠 클릭 / `X1` / `X2` 옆 버튼
- 여러 개 등록 가능 — 등록된 것 중 **아무거나** 누르면 작동합니다
- `ESC`를 누르면 등록 취소, 15초 지나면 자동 취소됩니다

> 💡 왼쪽/오른쪽 클릭은 게임에서도 쓰이니 **X1/X2 옆 버튼, CapsLock, F9**를 추천합니다.

### 2️⃣ 모드 선택

| 모드 | 동작 | 이런 때 |
|---|---|---|
| **누르고 있는 동안만** (기본) | 누르고 있는 동안만 낮아지고, 떼면 복원 | 팀원 말소리를 잠깐 들을 때 |
| **누를 때마다 전환** | 누를 때마다 낮추기 / 복원 토글 | 오래 조용히 하고 싶을 때 |

### 3️⃣ 볼륨 조절

| 설정 | 범위 | 기본값 |
|---|---|---|
| 낮춘 볼륨 | 0% (무음) ~ 50% | **10%** — 속삭이듯 작게 |
| 페이드 | 0ms (즉시) ~ 300ms | **0ms** |

`[지금 낮추기]`로 바로 테스트하고 `[원래대로]`로 복원할 수 있습니다.

### 4️⃣ 게임 목록 — 여러 게임 동시 지원

체크한 게임만 덕킹합니다. 기본 10개가 등록되어 있습니다.

| 게임 | 프로세스 | 게임 | 프로세스 |
|---|---|---|---|
| Valorant | `VALORANT-Win64-Shipping.exe` | Apex Legends | `r5apex.exe` |
| League of Legends | `League of Legends.exe` | Fortnite | `FortniteClient-Win64-Shipping.exe` |
| CS2 | `cs2.exe` | Eternal Return | `EternalReturn.exe` |
| PUBG | `TslGame.exe` | Escape from Tarkov | `EscapeFromTarkov.exe` |
| Overwatch 2 | `Overwatch.exe` | FC Online | `FO4Client.exe` |

exe명을 입력해 `[게임 추가]`로 어떤 게임이든 추가할 수 있습니다. `목록 외 앱도 자동 덕킹`을 켜면 목록에 없는 실행 중 앱 볼륨도 낮아집니다.

### 5️⃣ 백그라운드

- 닫기(X)를 누르면 **트레이로 숨겨져 계속 동작**합니다
- 트레이 아이콘 우클릭 → 열기 / 지금 볼륨 낮추기 / 종료
- `윈도우 시작 시 실행`을 켜면 부팅 때 자동 시작합니다
- 종료하거나 비활성화하면 게임 볼륨을 **원래대로 복원**하고 끝냅니다

## ⚙️ 어떻게 작동하나

```mermaid
flowchart LR
    A[⌨️ 키 / 🖱️ 마우스 버튼] -->|전역 훅 - 듣기만 함| B[GameDuck]
    B -->|Core Audio 세션 API| C[게임 프로세스 볼륨 ↓]
    C -->|버튼을 떼면| D[원래 볼륨으로 복원]
```

- Windows Core Audio 세션 API(`IAudioSessionManager2` / `ISimpleAudioVolume`)로 **게임 프로세스별 오디오 세션 볼륨만** 조절합니다.
- 게임에 DLL을 주입하지 않고, 게임 메모리를 읽지 않으며, 시스템 마스터 볼륨은 건드리지 않습니다.
- 트리거 키/마우스는 전역 로우레벨 훅(`WH_KEYBOARD_LL` / `WH_MOUSE_LL`)으로 **듣기만** 합니다. 입력을 가로채거나 막지 않으므로 게임 조작은 그대로 전달됩니다. (키 매핑을 새로 등록할 때 그 한 번의 입력만 게임으로 전달되지 않습니다.)
- 훅은 이벤트 방식이라 키를 안 누르면 CPU를 거의 안 씁니다. 폴링 없음. 게임 상태 스캔은 2초에 한 번만 합니다.

## 🔧 직접 빌드하기

Windows에서 `build.bat`을 실행하거나 (mingw-w64 필요):

```bat
x86_64-w64-mingw32-windres GameDuck.rc -O coff -o GameDuck.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -static-libgcc -static-libstdc++ main.cpp GameDuck.res -o GameDuck.exe -lcomctl32 -lgdi32 -luser32 -lshell32 -lole32 -luuid -lpsapi -lshlwapi
```

| 파일 | 설명 |
|---|---|
| `main.cpp` | 전체 소스 (단일 파일) |
| `GameDuck.ico` / `GameDuck.rc` / `GameDuck.exe.manifest` | 아이콘 · 리소스 · 매니페스트 |
| `build.bat` | Windows 빌드 스크립트 |
| `GameDuck.ini` | 실행 후 자동 생성되는 설정 파일 (exe 옆) |

## ❓ 문제 해결

<details>
<summary><b>키를 눌러도 안 낮아져요</b></summary>

상단 `[덕킹 활성화]` 체크를 확인하고, 게임이 목록에서 체크됐는지 확인하세요. 게임을 먼저 켠 뒤 `[실행 중인 게임 새로고침]`을 눌러보세요.

</details>

<details>
<summary><b>볼륨이 원래대로 안 돌아와요</b></summary>

`[원래대로]` 버튼을 누르세요. GameDuck을 종료하면 자동으로 복원됩니다.

</details>

<details>
<summary><b>관리자 권한 게임에서 안 돼요</b></summary>

GameDuck도 우클릭 → **관리자 권한으로 실행**하세요. 관리자 권한 게임의 오디오 세션은 일반 권한 프로세스에서 조절이 안 될 수 있습니다.

</details>

<details>
<summary><b>일부 게임에서 바로 반영이 안 돼요</b></summary>

일부 게임은 오디오 세션을 여러 개 만들거나 실행 직후 세션이 늦게 생깁니다. 게임 내 볼륨 설정도 함께 확인하세요.

</details>

---

<div align="center">

**Made for gamers who need to hear their team. 🎧**

MIT License · v1.0.0 (2026-11-19)

</div>
