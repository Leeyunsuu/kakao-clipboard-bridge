# KakaoTalk Clipboard Bridge

Paste GNOME screenshots and copied images into KakaoTalk running in Bottles over XWayland.

GNOME 기본 스크린샷으로 캡처하거나 브라우저에서 **이미지 복사**를 선택한 뒤 Bottles의 카카오톡 채팅창에서 `Ctrl+V`를 누르면 이미지를 붙여넣을 수 있습니다. 서비스는 원본 PNG와 함께 제공된 형식을 유지하고, 앱이 BMP를 요청할 때만 C/libpng 변환기로 24비트 BMP를 만들어 전달합니다. 이미지 크기는 유지하고, BMP의 투명 영역은 흰 배경과 합성합니다. CopyQ나 GNOME 확장은 필요하지 않습니다.

## 지원 환경

- 확인한 환경은 Fedora 44, GNOME 50 Wayland, Bottles의 Wine 11(XWayland)입니다.
- Bottles에서 카카오톡이 XWayland로 실행되어야 합니다. Wine의 Wayland 모드에서는 동작을 확인하지 않았습니다.
- 이 서비스는 PNG 데이터를 제공하는 클립보드 이미지를 처리합니다. 일반 텍스트, 파일 전송 전용 항목, 이미 BMP를 제공하는 항목은 건너뜁니다.

## 설치

GNOME에 로그인한 상태에서 이 저장소의 폴더를 열고 터미널에서 실행합니다.

```sh
sudo dnf install gcc pkgconf-pkg-config gtk4-devel libpng-devel xorg-x11-server-Xwayland

cc -std=c11 -O2 -Wall -Wextra kakao-clipboard-bridge.c kakao-png-to-bmp.c \
  -o kakao-clipboard-bridge $(pkg-config --cflags --libs gtk4 libpng)
install -Dm700 kakao-clipboard-bridge "$HOME/.local/bin/kakao-clipboard-bridge"
install -Dm644 kakao-clipboard-bridge.service "$HOME/.config/systemd/user/kakao-clipboard-bridge.service"
systemctl --user daemon-reload
systemctl --user enable --now kakao-clipboard-bridge.service
```

이미 서비스를 설치했다면 파일을 갱신한 뒤 `systemctl --user restart kakao-clipboard-bridge.service`를 실행합니다. 서비스는 GNOME에 로그인할 때 자동으로 시작합니다.

## 사용

1. `Print Screen`으로 원하는 영역을 캡처하거나, 브라우저에서 이미지를 우클릭해 **이미지 복사**를 선택합니다.
2. 카카오톡 채팅창에서 `Ctrl+V`를 누릅니다. 큰 캡처는 변환에 시간이 걸릴 수 있으니 바로 붙지 않으면 잠시 뒤 다시 시도합니다.

클립보드 변경 알림이 오면 원본 PNG와 HTML 등 함께 제공된 형식을 확보합니다. 다른 앱은 이 데이터를 그대로 요청할 수 있습니다. BMP는 요청할 때만 메모리에 만들고 응답을 마치면 해제합니다. 이미지를 별도 파일에 저장하거나 네트워크로 전송하지 않으며, 서비스 로그에는 변환 상태만 기록합니다.

동작하지 않으면 Bottles의 Wine Wayland 모드가 꺼져 있는지 확인하고 아래 명령으로 서비스 상태를 확인하세요.

```sh
systemctl --user status kakao-clipboard-bridge.service
journalctl --user -u kakao-clipboard-bridge.service -n 20
```

## 제거

```sh
systemctl --user disable --now kakao-clipboard-bridge.service
rm -- "$HOME/.config/systemd/user/kakao-clipboard-bridge.service" \
  "$HOME/.local/bin/kakao-clipboard-bridge"
systemctl --user daemon-reload
```
