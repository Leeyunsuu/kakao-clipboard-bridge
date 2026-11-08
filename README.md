# KakaoTalk Clipboard Bridge

Paste GNOME screenshots into KakaoTalk running in Bottles over XWayland.

GNOME 기본 스크린샷으로 캡처한 뒤 Bottles의 카카오톡 채팅창에서 `Ctrl+V`를 누르면 이미지를 붙여넣을 수 있습니다. 클립보드에 PNG만 들어오면 원본 PNG를 유지하면서 카카오톡에 전달할 BMP를 추가합니다. CopyQ나 GNOME 확장은 필요하지 않습니다.

## 지원 환경

- 확인한 환경은 Fedora 44, GNOME 50 Wayland, Bottles의 Wine 11(XWayland)입니다.
- Bottles에서 카카오톡이 XWayland로 실행되어야 합니다. Wine의 Wayland 모드에서는 동작을 확인하지 않았습니다.
- 이 서비스는 스크린샷을 포함해 PNG만 제공하는 클립보드 항목을 처리합니다. 텍스트, 파일, 여러 형식을 제공하는 항목은 건너뜁니다.

## 설치

GNOME에 로그인한 상태에서 이 저장소의 폴더를 열고 터미널에서 실행합니다.

```sh
sudo dnf install gtk4 python3-gobject ImageMagick xorg-x11-server-Xwayland

install -Dm700 kakao-clipboard-xwayland.py "$HOME/.local/bin/kakao-clipboard-xwayland.py"
install -Dm644 kakao-clipboard-bridge.service "$HOME/.config/systemd/user/kakao-clipboard-bridge.service"
systemctl --user daemon-reload
systemctl --user enable --now kakao-clipboard-bridge.service
```

이미 서비스를 설치했다면 파일을 갱신한 뒤 `systemctl --user restart kakao-clipboard-bridge.service`를 실행합니다. 서비스는 GNOME에 로그인할 때 자동으로 시작합니다.

## 사용

1. `Print Screen`으로 GNOME 캡처를 열고 원하는 영역을 캡처합니다.
2. 카카오톡 채팅창에서 `Ctrl+V`를 누릅니다. 큰 캡처는 변환에 시간이 걸릴 수 있으니 바로 붙지 않으면 잠시 뒤 다시 시도합니다.

변환은 클립보드 변경 알림을 받을 때만 실행합니다. 이미지를 별도 파일에 저장하거나 전송하지 않으며, 서비스 로그에는 변환 상태만 기록합니다.

동작하지 않으면 Bottles의 Wine Wayland 모드가 꺼져 있는지 확인하고 아래 명령으로 서비스 상태를 확인하세요.

```sh
systemctl --user status kakao-clipboard-bridge.service
journalctl --user -u kakao-clipboard-bridge.service -n 20
```

## 제거

```sh
systemctl --user disable --now kakao-clipboard-bridge.service
rm -- "$HOME/.config/systemd/user/kakao-clipboard-bridge.service" "$HOME/.local/bin/kakao-clipboard-xwayland.py"
systemctl --user daemon-reload
```
