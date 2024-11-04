# Ubuntu 18.04 이미지를 x86 아키텍처로 명시
FROM --platform=linux/amd64 ubuntu:18.04
# apt 패키지 목록 업데이트 및 기본 패키지 설치
RUN apt-get update && apt-get install -y \
    ufw \
    curl \
    vim \
    git \
    sudo \
    net-tools \
    iputils-ping \
    locales \
    # Pintos 개발에 필요한 추가 패키지들
    build-essential \
    gdb \
    qemu \
    qemu-system-x86 \
    python3 \
    gcc-multilib \
    # ssh 연결에 필요한 추가 패키지
    openssh-server \
    && apt-get clean
# 로케일 설정 (UTF-8 로케일 설정)
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8
# 사용자 추가 및 sudo 권한 부여
RUN useradd -ms /bin/bash ubuntu && adduser ubuntu sudo
# sudo 사용시 패스워드 없이 사용할 수 있도록 설정 (선택사항)
RUN echo "ubuntu ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
# root 비밀번호 변경
RUN echo "root:1q2w3e4r!" | chpasswd
# 사용자 비밀번호 변경
RUN echo "ubuntu:1q2w3e!" | chpasswd
# SSH 포트 노출
EXPOSE 22 80 8080 4500
# 홈 디렉토리로 워킹 디렉토리 설정
WORKDIR /home/ubuntu
# 사용자로 스위치
USER ubuntu
# 컨테이너 실행 유지
CMD ["tail", "-f", "/dev/null"]