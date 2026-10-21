; Licensed under the MIT License. Test boundary for MIR's global r12 binding.
_TEXT SEGMENT
PUBLIC mirnext_reference_invoke
mirnext_reference_invoke PROC FRAME
    push r12
    .pushreg r12
    push rbx
    .pushreg rbx
    sub rsp, 40
    .allocstack 40
    .endprolog
    mov rbx, rdx
    mov r12, 7
    call rcx
    test rbx, rbx
    jz done
    mov QWORD PTR [rbx], r12
done:
    add rsp, 40
    pop rbx
    pop r12
    ret
mirnext_reference_invoke ENDP
_TEXT ENDS
END
