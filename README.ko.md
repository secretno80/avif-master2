# AVIF-Master 기술 사양서 및 개발 가이드

**언어 / Language:** 한국어 | [English](README.md)

---

## [Part 1: 시스템 개요 및 아키텍처 설계]

### 1. 프로젝트 개요
AVIF-Master는 윈도우 환경에 맞춰진 로우레벨 하이브리드 변환 도구입니다.  
Win32 API 기반의 초경량 GUI, libarchive 라이브러리를 이용한 인메모리 스트림 처리, GPU VRAM 캐싱 및 OpenCL 전처리 파이프라인을 결합하여, 대량의 이미지 및 압축 파일 속 데이터를 지연 없이 초고속으로 처리하는 것을 목표로 하며 아래와 같은 라이브러리 및 외부도구를 사용합니다.

- libarchive 사용으로 별도 외부 라이브러리 및 도구 사용 X  
  (사용 시 반드시 github의 최신 README.md 파일 참조할 것 https://github.com/libarchive/libarchive)
- avifenc, avifdec 을 이용한 변환

### 2. 마스터-슬레이브(Master-Slave) 인스턴스 관리 설계 (데이터 누락 방지)
탐색기 컨텍스트 메뉴를 통해 다수의 파일이 유입될 때 발생할 수 있는 데이터 누락을 방지하기 위한 동기화 아키텍처입니다.

#### 2.1 적응형 대기 시스템 (Adaptive Settling Time)
탐색기에서 대량의 항목을 선택하고 실행할 때, 윈도우 OS는 인자(Argument)를 여러 개의 프로세스로 나누어 실행하거나 순차적으로 실행할 수 있습니다.  
마스터 프로세스가 이를 완벽하게 취합하기 위해 다음 로직을 사용합니다.

- **마스터 선점**: 첫 번째 실행된 프로세스가 Global Mutex(wWinMain에서 CreateMutexW 활용)를 선점하여 Master가 됩니다. GUI 메시지 루프를 유지합니다.
- **슬레이브 행동**: 후속 실행된 프로세스(Slave)들은 자신의 경로 인자를 Master의 뮤텍스를 소유할 수 없을 때, Temp 폴더 내에 고유 PID 명명 규칙을 가진 .args 파일에 기록하고 즉시 종료됩니다.
- **적응형 대기**: Master는 wWinMain 진입 직후, .args 파일이 새로 생성되는지 감시(Polling)하는 타이머 기반 수집 단계를 가집니다. 마지막 .args 파일이 생성된 시점부터 300ms ~ 500ms 동안 추가 유입이 없는지 확인합니다.  
  이 대기 시간이 충족되면 모든 .args 파일을 일괄 취합하여 그리드(GridView)에 원자적으로 등록하고, 워커 스레드를 가동합니다.

#### 2.2 IPC (프로세스 간 통신)
초기 대기 시스템 가동 이후 프로그램이 실행 중일 때 추가로 실행된 건에 대해서는 WM_COPYDATA 메시지를 통해 마스터 윈도우의 그리드에 항목을 추가합니다.

#### 2.3 자동 변환 모드 (Auto-Start on File Collection)
명령줄 인자 `/auto` 또는 `--auto-convert`를 통해 파일 수집 완료 후 자동으로 변환을 시작할 수 있습니다.
- 파일 수집 시작 → 모든 파일 수신 대기 (Settling Time) → 수집 완료 → **자동으로 변환 시작**
- 컨텍스트 메뉴에서 "Convert with AVIF-Master (Start Now)" 옵션을 선택하면 자동 모드 활성화
- 일반 "Convert with AVIF-Master" 옵션은 파일만 추가하고 사용자 확인 후 시작

---

## [Part 2: 상세 기능 및 UI 명세]

### 3. 메인 화면 그리드(ListView) 컬럼 구성 및 동작
리소스 소모를 최소화하기 위해 Virtual ListView (LVS_OWNERDATA 모드)를 사용합니다. 각 컬럼은 워커 스레드로부터 상태 플래그를 받아 실시간 업데이트됩니다.

| 컬럼 | 설명 |
|------|------|
| 이름 | 파일명 또는 압축파일명 표시 (SHGetFileInfo를 이용해 시스템 아이콘 Lazy Loading) |
| 형식 | 대상의 형식(JPG, PNG, WebP, ZIP, 7Z 등) 표시 (확장자 기반 단순 판별) |
| 원본 용량 | 변환 전 데이터 크기 (KB/MB/GB 단위, GetFileAttributesExW 사용) |
| 상태 | 실시간 상태 표시 (대기 / 수집중... / 변환중(%) / 완료 / 실패) |
| 결과 용량 | 변환 완료 후 실제 생성된 AVIF 파일 크기 |
| 압축률 | 원본 대비 절감 비율 (%) = (원본 용량 - 결과 용량) / 원본 용량 × 100 |

### 4. 압축 파일 처리 전략
압축 파일(ZIP/7Z) 등록 시, 내부 파일을 전개하지 않고 **하나의 행(Row)**으로 등록합니다.  
상태 칸에 `[변환중] 45/200`과 같이 내부 진행도를 텍스트로 업데이트하여 UI 부하를 줄입니다.  
변환 완료 후 재압축(Repacking) 로직: 원본 압축 구조와 동일하게 `.zip` 또는 `.7z`로 재포장하며, 변환 완료된 AVIF 파일만 아카이브 내에서 교체합니다.

#### 4.1 압축 파일 처리 상세 프로세스
1. **압축 해제**: 임시 폴더에 전개 (진행상태: "압축 해제 중...")
2. **이미지 변환**: 각 이미지를 in-place 변환 (진행상태: "변환 중 X/N")
3. **원본 백업**: "Preserve archive backup" 옵션 활성화 시 원본을 `_backup` 이름으로 보존
4. **재압축**: 같은 포맷(ZIP/7Z)으로 재압축 (진행상태: "재압축 중...")
5. **로깅**: "Save conversion log" 옵션 활성화 시 변환 결과 기록
   - 로그 위치: `원본_디렉토리\AVIF_Conversion_Logs\YYYYMMDD_HHMMSS_convert.log`
   - 기록 내용: 파일명, 시간, 성공/실패 통계, 실패 파일 목록

---

## [Part 3: UI 레이아웃 및 옵션 상세 설정]

### 5. 메인 화면 구성 및 요소 별 기능
메인 윈도우는 크게 세 개의 기능 영역으로 나뉩니다.

#### 5.1 그리드 영역 (상단)
- 전용 가상 리스트뷰(`hWndList = CreateWindowExW(..., WC_LISTVIEWW, ...)`) 배치. `LVS_OWNERDATA` 사용.
- 개별 체크박스를 통한 배치 선택 지원.
- `WM_DROPFILES` 메시지를 처리하여 Drag & Drop 기능 구현.

#### 5.2 성능 및 옵션 제어 영역 (우측 패널)
Performance 탭과 File Management 탭으로 구성된 탭 컨트롤 패널입니다.

**Performance 탭**
- **하드웨어 실행 방식 토글**: [CPU Only] (정밀도 우선) / [CPU + GPU 하이브리드] (그래픽카드의 OpenCL 전처리 파이프라인 가속)
- **GPU Caching 토글**: GPU 모드 선택 시 활성화. 비동기 VRAM 사전 캐싱(Asynchronous Prefetching) 로직 활성화.
- **CPU 전략 제어 (Batching)**: 동시에 처리할 이미지 개수(Concurrent Jobs)와 이미지당 할당 스레드 수(Threads per Job)를 하드웨어에 맞춰 최적화 분할.
- **SIMD/Assembly 토글**: SVT-AV1 인코더의 어셈블리 최적화를 위한 CPU 명령어를 탐색하고 플래그 강제 활성화/비활성화.
- **Quality Preset**:
  - ⚡ **Fast** (Speed Priority): 빠른 변환, 낮은 압축률 (`avifenc --preset 8`)
  - ⚙️ **Normal** (Balanced): 기본값, 성능과 품질의 균형 (`avifenc --preset 6`)
  - 🎯 **High** (Compression Priority): 느린 변환, 높은 압축률과 품질 (`avifenc --preset 4`)

**File Management 탭**
- **출력 경로 설정**: [원본 폴더] / [특정 폴더 지정] / [하위 폴더 구조 유지] (Radio buttons)
- **파일수정일유지**: 변환 결과 파일의 수정일을 원본 파일의 수정일로 맞춥니다. 체크 해제 시 결과 파일 수정일은 변환 시각으로 유지됩니다.
- **중복 변환 방지 토글**: 동일 파일명 또는 이미 변환된 `.avif`가 존재할 경우 건너뛰기 옵션.
- **노이즈 제거 필터 토글**: 활성화 시 GPU OpenCL fastNlMeansDenoising 필터를 사용하여 텍스처 노이즈 제거.
- **리사이징 설정**: LANCZOS3 리사이징 필터 적용 및 특정 해상도 설정.
- **자동 종료 토글**: 변환 완료 후 자동으로 프로그램 종료.
- **원본 아카이브 백업**: 압축 파일 변환 시 원본을 `_backup` 이름으로 보존 (ZIP/7Z 전용).
- **변환 로그 저장**: 변환 결과를 타임스탬프 포함 로그 파일로 기록하며, 성공 완료 시 `.log` 파일은 자동 삭제됩니다.
- **프로파일 저장**: 현재 설정 구성을 저장(Save)하거나 불러오기(Load) 기능.

#### 5.3 상태 표시 영역 (하단 상태바)
하단 전체 영역 Smooth Progress Bar 배치.
- 좌측: 전체 진행 상태 표시 (Idle / 진행중... / 완료 / 실패)
- 우측: 현재 처리 속도(TPS/FPS), 남은 시간(ETA) 표시

### 6. 완료 리포트 대시보드 (Success Report)
작업이 완료되면 나타나는 요약 보고창입니다.
- **총 진행 시간**: 밀리초(ms) 단위까지 기록
- **성공/실패 건수**: 상세 오류 로그 링크 제공
- **용량 변화**: 원본 전체 용량 → 결과 전체 용량 (절감 용량 % 및 bar chart로 표시)

---

## [Part 4: 빌드, 설치 및 배포]

### 7. 컨텍스트 메뉴 통합
우클릭 컨텍스트 메뉴 이름: **"AVIF-Master로 고속 변환"**

동작: HKCR 레지스트리 키(`*\shell` 및 `Directory\Background\shell`) 설정을 통해 `%1` 인자를 마스터 프로세스에 전달. Adaptive Settling Time 로직(300~500ms 지연)을 통해 모든 대상 수집 후 그리드 등록.

### 8. 빌드 및 배포 (설치 파일 생성)
- 윈도우 프로그램 추가/제거 및 설치된 파일의 실행 파일과 함께 `.ico` 파일을 사용합니다.
- `build.bat` 파일을 통해 빌드를 자동화하고, 빌드된 파일을 Inno Setup 스크립트(`setup.iss`)로 설치 파일을 생성합니다.
- 프로그램 삭제 시 파일 및 레지스트리에 남은 내용이 없도록 완전 제거합니다.

### 9. 파일수정일유지 기능 동작 및 수동 검증 절차

#### 동작 요약
- 파일수정일유지가 체크되어 있으면 변환 성공 후 결과 파일의 수정일을 원본 파일 수정일로 설정합니다.
- 체크가 해제되어 있으면 결과 파일의 수정일은 일반 파일 생성 규칙(변환 시점)대로 기록됩니다.
- 수정일 읽기 또는 쓰기에 실패하면 상태 문자열에 실패 안내가 추가됩니다.
- 변환이 성공하면 해당 항목의 `.log` 파일은 자동으로 삭제됩니다.

#### 수동 검증 절차 (GUI)
1. 원본 이미지 파일 1개를 준비하고 파일 속성에서 수정일을 확인합니다.
2. 프로그램을 실행하고 파일을 등록한 뒤 파일수정일유지를 체크합니다.
3. 변환을 완료한 뒤 결과 파일의 수정일이 원본과 같은지 확인합니다.
4. 같은 방식으로 파일수정일유지를 해제한 상태에서 다시 변환해 결과 파일 수정일이 현재 시각으로 기록되는지 확인합니다.

#### 수동 검증 절차 (자동 변환 모드)
1. 레지스트리 `HKCU\Software\AVIFMaster2`의 `KeepModifiedTime` 값을 `1`로 설정합니다.
2. `AVIFMaster2.exe /auto 원본파일경로` 로 실행 후 결과 파일 수정일을 확인합니다.
3. `KeepModifiedTime` 값을 `0`으로 바꾸고 동일 절차를 반복하여 ON/OFF 차이를 비교합니다.

---

## Third-Party Licenses (오픈소스 라이선스)

이 프로젝트는 다음 오픈소스 라이브러리 및 도구를 포함하거나 사용합니다.  
전문 라이선스 텍스트는 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) 파일을 참조하십시오.

| 라이브러리 | 버전     | 라이선스     | 용도                                 |
|-----------|----------|--------------|--------------------------------------|
| libavif   | 1.4.0    | BSD 2-Clause | AVIF 인코딩/디코딩 실행 파일 (lib/)  |
| libaom    | 3.13.1   | BSD 2-Clause | AV1 인코더 (avifenc 내 번들)         |
| dav1d     | 1.5.3    | BSD 2-Clause | AV1 디코더 (avifenc/avifdec 내 번들) |
| libyuv    | rev.1922 | BSD 3-Clause | YUV 색공간 처리 (libavif 내 번들)    |
| libwebp   | (source) | BSD 3-Clause | WebP 지원 (Dependencies/libwebp/)    |

## 크레딧

- 저장소 게시 계정: secretno80
- 구현 및 문서화의 대부분은 AI 보조로 작성되었습니다.
- 사용한 AI 도구: GitHub Copilot, Claude

## License

This project is licensed under the [MIT License](LICENSE).
