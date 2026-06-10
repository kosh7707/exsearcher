# exsearcher 설계 문서

인덱싱 기반 초고속 파일명 검색 도구. Everything 대체품.

- 타겟 OS: Windows 전용
- 핵심 차별점: RaiDrive 등으로 마운트된 네트워크 드라이브(Z:\ 등) 색인 지원 (Everything은 미지원)
- 철학: 기능 최소주의. 검색창 하나, 결과 리스트 하나.

## 사용자 전제

- 비개발자. 설치를 싫어함.
- **포터블 배포**: zip 풀어서 실행, 폴더 삭제로 완전 제거.
- 부트스트랩 < 1초 (실행 → 검색 타이핑 가능).

## 핵심 결정 사항

| 항목 | 결정 | 근거 |
|---|---|---|
| 스택 | C++20 + Qt6 Widgets | 저메모리(~30MB), 즉시 시작. UI는 검색창+리스트가 전부라 Electron 장점 없음 |
| 인덱싱 | 병렬 BFS 크롤 단일 경로 | 주 타겟(네트워크 드라이브)이 MFT 불가. 모든 볼륨 동일 처리. 관리자 권한 불필요 |
| 색인 트리거 | **수동 (드라이브 칩 클릭)** | 사용자는 보통 특정 드라이브(Z:)만 필요. 자동 전체 크롤은 낭비 + AV 행위 탐지 자극 |
| MFT/USN | v2 옵션으로 보류 | 크롤 + 스냅샷으로 체감 속도 충분. 필요 시 NTFS 전용 가속으로 추가 |
| 권한 | 일반 유저 | raw 볼륨 접근 안 함 |
| 검색 범위 | 파일명만 | 내용 검색(FTS)은 스코프 외 |
| 영속화 | 커스텀 바이너리 스냅샷 (data/index.exsdb) | 시작 시 로드 → 재크롤 없이 즉시 검색. 크롤/제거 완료 시마다 원자적 저장(tmp+rename) |
| 진행률 | BFS 큐 잔량 기반 추정 | 처리/(처리+대기) 디렉토리 비율 — 크롤이 진행될수록 정확해짐. 현재 경로 동시 표시 |
| 디자인 | 라이트, Windows 11 Fluent 계열 | 사용자 눈 피로 + "AI 슬롭"(다크+보라 그라데이션) 탈피. 단일 액센트 #0F6CBD, 그라데이션/글로우 금지 |
| 타이틀바 | 커스텀 (frameless + WM_NCHITTEST/WM_NCCALCSIZE) | Win11 스냅 레이아웃(HTMAXBUTTON)·DWM 그림자·둥근 모서리 유지 |

## 아키텍처

```
exsearcher.exe (단일 프로세스, 일반 권한)
│
├─ app/   Qt6 Widgets UI
│   ├─ 검색창: search-as-you-type, 디바운스 ~30ms
│   └─ 결과: QTableView 가상화 + 커스텀 모델 (수백만 행 대응)
│
└─ core/  Qt 비의존 C++20 라이브러리 (단독 테스트 가능)
    ├─ Index         인메모리 인덱스
    ├─ CrawlIndexer  볼륨당 1개. 병렬 BFS 크롤
    │                (FindFirstFileEx + FIND_FIRST_EX_LARGE_FETCH, 스레드 4~8)
    ├─ Watcher       로컬 볼륨: ReadDirectoryChangesW (루트 재귀 감시)
    │                네트워크 볼륨: 주기 풀 리스캔 → diff → 원자적 스왑
    ├─ SearchEngine  멀티스레드 이름 버퍼 스캔
    └─ Snapshot      .exsdb 저장/로드
```

## 인덱스 구조 (Everything 방식)

```cpp
struct FileEntry {
    uint32_t nameOffset;   // 이름 버퍼 내 위치
    uint16_t nameLen;
    uint32_t parentIdx;    // 디렉토리 엔트리 인덱스 → full path 재구성
    uint64_t size;
    uint64_t mtime;
    uint32_t attr;         // dir 여부, hidden 등
};
```

- 이름은 UTF-8 연속 버퍼에 저장 + lowercase 미러 버퍼(대소문자 무시 검색용)
- 메모리 예상: 파일 200만 개 기준 ~150MB
- 검색: lowercase 버퍼 substring 스캔, 스레드별 청크 분할. 200만 건 수십 ms

### 검색 문법 (v1)

- 공백 구분 토큰 = AND 조건
- substring 매치, 대소문자 무시
- 그 외 문법 없음 (의도적 단순화)

## 볼륨별 전략

| 볼륨 | 초기 색인 | 갱신 |
|---|---|---|
| 로컬 (C:, D: 등) | 병렬 BFS 크롤 (SSD 100만 파일 ~30초–2분) | ReadDirectoryChangesW 실시간. 버퍼 오버플로 시 해당 볼륨 리스캔 fallback |
| 네트워크 (Z: 등) | 병렬 BFS 크롤 (네트워크 속도 의존, 진행률 UI 필수) | 주기 풀 리스캔 (간격 설정 가능) + 수동 새로고침 버튼 |

### 시작 흐름

1. `data/index.exsdb` 로드 → UI 즉시 검색 가능 (< 1초). 자동 크롤 없음
2. 미색인 드라이브는 칩 클릭 시에만 색인 (해당 드라이브만 append 크롤)
3. 칩 우클릭: 재색인(removeRoot + 크롤) / 색인에서 제거. 크롤·제거 완료 시마다 스냅샷 저장
4. 변경 감지(M3 예정): 로컬 RDCW + 네트워크 주기 리스캔

## 패키징 / 배포

```
exsearcher/              ← zip 배포, 풀면 끝
├─ exsearcher.exe
├─ Qt6*.dll, platforms/  ← windeployqt 산출물
├─ settings.ini          ← 실행 시 생성 (레지스트리 미사용)
└─ data/index.exsdb      ← 실행 시 생성
```

- 모든 상태를 exe 폴더 내부에 저장 → **폴더 삭제 = 완전 제거**
- 레지스트리, %APPDATA%, 서비스, 설치 프로그램 일절 없음
- 크기 ~30-40MB
- 자동 시작 기능은 레지스트리 Run 키가 필요하므로 기본 OFF, 활성화 시 경고 표시
- 빌드: CMake + MSVC + Qt6 동적 링크 (LGPL). 단일 exe(정적 링크)는 추후 검토

## 마일스톤

1. **M1**: core 라이브러리 — 병렬 BFS 크롤 + 인메모리 인덱스 + 검색 엔진 + CLI 검증 도구
2. **M2**: Qt UI — 검색창, 가상화 결과 리스트, 더블클릭 실행 / 탐색기에서 열기
3. **M3**: 갱신 — ReadDirectoryChangesW(로컬) + 주기 리스캔(네트워크) + diff 원자 스왑
4. **M4**: 스냅샷 영속화(.exsdb), 설정창, 트레이 상주, 전역 단축키

## 리스크

- RaiDrive 크롤 속도는 네트워크/백엔드(SMB·WebDAV·FTP) 의존 — 첫 크롤 수 분 가능, 진행률 표시 필수
- ReadDirectoryChangesW는 네트워크 드라이브에서 신뢰 불가 → 네트워크는 폴링 리스캔만 사용
- 정렬(크기/날짜)은 검색 결과에만 lazy 적용, 전체 인덱스 사전 정렬 없음
