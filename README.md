# exsearcher

네트워크 드라이브(NAS)까지 색인하는 Windows용 초고속 파일명 검색기. 포터블 — 압축 풀고 실행, 폴더 삭제로 제거.

## 사용법

1. `exsearcher.exe` 실행
2. 검색창 아래 `+`가 붙은 드라이브 칩 클릭 → 색인
3. 검색. 창을 닫아도 트레이에 상주 (종료: 트레이 우클릭 → 종료)

- 칩 클릭: 검색 대상 포함/제외 · 칩 우클릭: 재색인/제거
- 결과 더블클릭: 열기 · 우클릭: 폴더에서 열기, 경로 복사

## 검색 문법

공백으로 구분, 전부 AND, 대소문자 무시.

| 토큰 | 의미 | 예시 |
|---|---|---|
| `보고서` | 파일명에 포함 | `계약서 2024` |
| `.pdf` | 확장자 | `2024 .hwp` |
| `사진\` | 상위 폴더명에 포함 | `사진\ .jpg`, `다운로드\ setup .exe` |
| `"..."` | 공백 포함 묶기 | `"ZZ. Sehwa"\` (이름에 공백 있는 폴더) |

## 설정 (exe 옆 settings.ini)

| 키 | 기본값 | 의미 |
|---|---|---|
| `index/rescanMinutes` | `30` | 네트워크 드라이브 자동 리스캔 간격(분), `0`=끔 |
| `index/watchLocal` | `true` | 로컬 드라이브 실시간 변경 감지 |

## 빌드

```
cmake -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x/msvc2022_64
cmake --build build --config Release --target deploy   # build/dist/exsearcher/
```

Qt 6 (LGPLv3, 동적 링크) · 설계: [DESIGN.md](DESIGN.md)
