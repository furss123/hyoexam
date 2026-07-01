# HyoExam

시험(모의고사·내신) 시간표와 실시간 시계를 TV/모니터로 송출하는 Windows 앱.

- 실시간 시계는 `www.naver.com`의 HTTP `Date` 응답 헤더를 10초마다 조회해
  동기화합니다 (표준 NTP가 아닌, 브라우저에서 보는 "네이버 시간"과 일치).
- 순수 Win32 + Direct2D/DirectWrite — 프레임워크 오버헤드 없이 실행 파일
  1개(수 MB), 시작 즉시 렌더링, GC 없음.

## 조작

기본은 **전체화면(학생용 화면)** — 설정/전체화면 아이콘과 저작권 표시가 전부
숨겨지고 시계·시간표만 보입니다. `F11` 또는 `Esc`로 창 모드로 나오면 상단에
전역 툴바(⚙설정 · ⛶전체화면)가 나타나고, 시간표 편집 기능이 항상 켜져 있습니다.

| 동작 | 방법 |
|---|---|
| 창 모드 ↔ 전체화면 | `F11`, 또는 상단 툴바의 전체화면 아이콘 |
| 전체화면 나가기 | `Esc` (또는 `F11`) — 전체화면으로 다시 들어가면 설정/편집 팝업은 자동으로 닫힘 |
| 설정 열기/닫기 | `F2`, 또는 상단 툴바의 톱니바퀴 아이콘 |

### 시간표 편집 (창 모드에서 항상 가능 — 별도 켜기 불필요)

- **레이아웃**: 좌(시계)/우(시간표) 패널 사이 경계선을 드래그해 비율 조절 (40%~85%)
- **교시 추가/수정**: 우측 카드의 교시 행을 클릭 → 팝업에서 교시명·과목·시작·종료
  입력 후 저장 (소요시간은 자동 계산). 각 행의 ✕ 아이콘으로 즉시 삭제,
  `+ 교시 추가` 버튼으로 새 교시 추가
- **안내 문구**: 하단 안내문 옆 연필 아이콘 → 한 줄에 하나씩 입력해 편집
- 모든 편집은 즉시 `data/schedules.json`에 저장됩니다

### 설정 (톱니바퀴)

시험 유형(모의고사/내신) 선택, 테마(다크/라이트/자동). 폰트는 Windows 10/11에
기본 내장된 **맑은 고딕**으로 고정되어 있어 별도 설치나 선택이 필요 없습니다.

## 빌드

Visual Studio 2022 Build Tools(C++ 데스크톱 개발 워크로드)와 CMake 필요.

```powershell
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
```

빌드 결과: `build\HyoExam.exe`. 실행 파일 옆에 `data\schedules.json`이 있어야 합니다
(CMake가 자동 복사하지 않으므로, 배포 시 `data\` 폴더를 exe와 함께 배치할 것).

## 데이터 파일

- `data/schedules.json` — 시험 유형별 교시/시간/과목/쉬는시간/안내문구.
  구조는 파일 내 `mock_exam`(모의고사), `midterm`(내신) 예시 참고. 학교마다
  시간표가 다르므로 각 학교에 맞게 직접 수정.
- `%APPDATA%\HyoExam\settings.json` — 테마·마지막 선택 시험 유형
  (앱이 자동 생성/저장).

## 브랜드 자산

`assets/gen_icon.py` (Pillow 필요)로 HyoT 브랜드 가이드에 맞는 아이콘을
재생성할 수 있습니다: 대각선 그라데이션(#4A9FE0→#2B7CC7), 흰색 단일 글리프,
투명 배경. 산출물: `assets/hyoexam.ico` (실행 파일 아이콘), `data/icon.webp`
(hyot.dev 등록용 512×512).

UI 폰트는 Windows 10/11 기본 내장 **맑은 고딕**으로 고정(별도 설치 불필요).
숫자/버전 표기만 브랜드 규정에 따라 JetBrains Mono를 사용합니다.

## hyot.dev 등록

`data/icon.webp`가 준비되면 hyot.dev 저장소의
`data/software/hyoexam/meta.json` + `banner.webp` + `releases.json`을
브랜드 가이드 템플릿대로 추가 후 `npm run validate:schemas && npm run build`.

```
HyoExam v1.0.0 | © 2026 HyoT. All rights reserved.
```
