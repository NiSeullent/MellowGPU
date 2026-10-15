# MellowGPU

Intel·AMD·NVIDIA를 위한 macOS GPU 드라이버 플랫폼입니다. 기존 Mellow의
이력과 포팅 자산을 보존하면서 공통 수명 관리, 네이티브 동기화, Metal 3
적합성, 업스트림 갱신 계약을 중심으로 구조를 개편합니다.

**지원 기준은 Metal 3 전체 기능과 WindowServer·앱·게임의 실제 GPU 가속입니다.**
장치 인식, 빌드, 모델 시험이나 offscreen 렌더링만 통과한 카드는 지원 목록에
등록하지 않습니다. CPU 래스터화와 CPU 픽셀 복사는 지원 판정에 사용할 수 없습니다.

현재는 **0.5.1-dev 개발 단계이며, 전체 조건을 검증한 GPU/OS 조합은 0개**입니다.
`MellowGPU.kext` 개발 빌드는 완성된 Metal 3 드라이버를 뜻하지 않습니다.
기존 GL/CL 실험은 연구 자산으로 보존하며 새 제품의 가속 증거로 승격하지 않습니다.

## 설계와 구현

- [아키텍처·적합성 명세](docs/MELLOWGPU-SPEC.md): 공통 코어, 벤더/OS 경계,
  메모리·펜스·디스플레이·종료·절전 계약과 전체 지원 조건.
- [구현 계획](docs/MELLOWGPU-IMPLEMENTATION.md): 병렬 구현 단위와 검증·배포 순서.
- [Metal 3 프로필](porting/metal3-profile.json): 필수 관문과 실제 지원 목록.
- [수명 관리 계약](docs/MELLOWGPU-LIFECYCLE.md),
  [소스·명세 갱신 도구](docs/MELLOWGPU-UPSTREAMS.md): 구현 경계와 검증 방법.
- [네이티브 커널 제어 경로](docs/MELLOWGPU-NATIVE-CONTROL.md),
  [Metal 선언·구현 검토 목록](docs/MELLOWGPU-METAL-API.md),
  [소스·패치 후보 생성](docs/MELLOWGPU-CANDIDATES.md).
- [기존 Mellow 문서](docs/LEGACY-MELLOW-README.md): 이전 구현과 증거의 원래 범위.

| 영역 | 새 구조 | 현재 범위 |
| --- | --- | --- |
| 공통 코어 | 장치 세대, 작업·핀·제출·공개·디스플레이 lease | 호스트에서 시험 가능한 계약 구현 |
| 커널 제어 경로 | 세 벤더 PCI 조회, 원래 task 전용 공유 RAM 할당·매핑·해제 | 실제 IOKit 코드·네이티브 빌드; 실기 미검증, GPU VM·제출 미구현 |
| Metal 3 지원 판정 | 정확한 소스/OS/명세 조합과 전체 기능·시스템 증거 | 정책 구현; 네이티브 인증·전체 API 구현은 진행 중 |
| Intel | i915/xe ↔ ANV | 기존 포팅 자산 및 새 공통 계약 연결 대상 |
| AMD | amdgpu/display ↔ RADV | 병렬 포팅 대상; 전체 가속 미검증 |
| NVIDIA | Nouveau ↔ NVK | 병렬 포팅 대상; open RM/GSP는 별도 ABI 연구 |
| 소스 갱신 | 불변 커밋·Git blob·라이선스·ABI·명세 추적 | 검증·탐지·정확한 소스/패치 적용 후보 생성; 빌드 승인·승격 없음 |
| 배포 제품 | `MellowGPU.kext` / `MellowGPU` | 개발용 빌드; 시스템 설치·가속 성공과 구분 |

NVIDIA open RM과 Nouveau/NVK의 객체·채널·펜스 ABI는 서로 바꿔 쓸 수 없습니다.
드라이버·사용자 공간·펌웨어·패치·설정은 검증한 조합으로 함께 고정합니다.

## 빌드와 시험

macOS와 전체 Xcode에서 실제 x86_64 커널 제품을 빌드합니다. 스크립트는 설치나
로드, EFI/NVRAM 변경을 수행하지 않습니다.

```sh
bash Tools/cross-build-native.sh Release
python3 Tools/run-gpu-core-tests.py --sanitize --out build/gpu-core
python3 Tools/run-metal3-profile-tests.py --sanitize --out build/metal3-profile
python3 Tools/run-control-tests.py --sanitize --out build/control-protocol
python3 Tools/run-native-control-tests.py --sanitize --out build/native-control
python3 Tools/run-probe-control-tests.py --sanitize --out build/control-probe
python3 tests/metal_api_inventory_tests.py
python3 Tools/run-platform-tests.py --sanitize --out build/platform-tests
python3 -m unittest discover -s tests -p 'test_mellow*.py' -v
```

릴리스용 파일은 커밋된 깨끗한 소스에서 새로 빌드하고 패키징합니다.
아래 출력 디렉터리는 기존에 없어야 합니다.

```sh
python3 Tools/build-mellowgpu-development.py --out build/development-native
python3 Tools/package-mellowgpu-development.py \
  --bundle build/development-native/artifact/MellowGPU.kext \
  --structural-report build/development-native/structural-report.json \
  --build-evidence build/development-native/build-result.json \
  --out build/development-package
```

이 경로는 Git 커밋·전체 추적 소스·빌드 도구·서명된 실제 바이너리·정적 검사
기록을 묶습니다. 소스 변경, 다른 빌드의 바이너리, 이전 제품 이름과 혼합된
번들은 거부합니다. 임시 파일 검사는 macOS의 심볼릭 링크 경로를 피하려면
`TMPDIR=/private/tmp`로 실행할 수 있습니다.

공통 코어와 프로필 시험은 합성 입력에 대한 실제 C++ 코드 시험입니다. GPU 실행,
Metal 3 적합성 또는 WindowServer 가속을 인증하지 않습니다. 네이티브 전체 API,
IOSurface 공유, GPU 펜스, 실제 scanout, 앱·게임·복구·성능 시험은 별도 관문입니다.

새 커널 제어 경로는 Darwin 24에서 `-mellowgpudev`를 명시한 개발 모드로
한정합니다. 이 모드에서는 기존 그래픽 패치 진입점을 건너뜁니다. 실제
GPU 명령이나 CPU 렌더러를 실행하지 않으며, 공유 RAM은 네이티브 리소스
경계를 연결하기 위한 기반입니다. 사용자 공간 진단 프로그램은 다음과 같이
빌드할 수 있습니다. 기본 실행은 조회, `--selftest`는 4 KiB 공유 메모리 시험입니다.

```sh
xcrun clang++ -std=c++17 -Wall -Wextra -Werror \
  -arch x86_64 -arch arm64 -mmacosx-version-min=13.0 \
  Userspace/mellowgpu-control.cpp -framework IOKit -framework CoreFoundation \
  -o build/mellowgpu-control
```

이 프로그램은 해당 개발 서비스가 실제 로드되어야 동작합니다. 서비스가 없는
호스트에서는 종료 코드 2와 `no_native_control_service`를 반환합니다.

## 업데이트 원칙

새 커밋과 명세 변경을 탐지하고 정확한 Git 소속·내용·ABI·설정·라이선스를
검사한 후보를 만듭니다. 패치·빌드·회귀 시험 후에도 해당 조합의 실제 GPU 검증이
없으면 지원 목록과 설치 제품을 자동 승격하지 않습니다. 현재 하드웨어 자동
인증·프로덕션 자동 배포는 구현되지 않았습니다.

## 개발 릴리스와 이전

새 제품 이름은 `MellowGPU.kext`, 실행 파일은 `MellowGPU`, 번들 ID는
`com.NiSeullent.MellowGPU`입니다. 이전 `Mellow.kext`와 동시에 로드하지 않습니다.
기존 릴리스와 검증 기록의 이름은 역사적 식별자로 보존합니다.

개발 prerelease에는 실제 빌드 결과, 체크섬, 소스 커밋, 명세와 구현 상태를
포함합니다. 실제 지원 GPU가 없는 개발 빌드를 일반 사용자용 가속 드라이버로
소개하지 않습니다.

## 출처와 라이선스

구조 참고: [NullMoth](https://github.com/nullmoth/nvidia-macos-driver),
[Cobalt64](https://github.com/Almosst-DEV/Cobalt64).
Metal 기준: [Apple 공식 명세](https://developer.apple.com/metal/resources/).

기존 [LICENSE](LICENSE), [NOTICE](NOTICE), 소스별 고지와 [LICENSES](LICENSES)를
유지합니다. 참고 프로젝트의 코드를 일괄 복사하거나 다른 라이선스로 표시하지
않습니다. 원본 Mellow/NootedGreen 계보와 벤더·SDK·펌웨어별 조건은 각각 적용됩니다.
