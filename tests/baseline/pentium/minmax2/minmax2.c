void test(int param1);

// address: 0x804835d
int main(int argc, char *argv[], char *envp[]) {
    test(-5);
    test(-2);
    test(0);
    test(argc);
    test(5);
    return 0;
}

// address: 0x8048328
void test(int param1) {
    int local0; 		// m[esp + 4]
    int local3; 		// param1{35}
    int local4; 		// local0{36}

    local3 = param1;
    if (param1 < -2) {
        local0 = -2;
        local3 = local0;
    }
    param1 = local3;
    local4 = param1;
    if (param1 > 3) {
        local0 = 3;
        local4 = local0;
    }
    local0 = local4;
    printf("MinMax result %d\n", local0);
    return;
}

