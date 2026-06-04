mouse_ps2_input_listener: mouse_ps2_input_listener {
    compatible = "zmk,input-listener-ps2";
    status = "okay";
    device = <&mouse_ps2>;

    /* 移动 TP 时自动激活 layer 3（鼠标层） */
    layer-toggle = <3>;

    /* 需要持续移动多久才激活（ms） */
    layer-toggle-delay-ms = <250>;

    /* 停止移动多久后自动关闭（ms） */
    layer-toggle-timeout-ms = <250>;
};
