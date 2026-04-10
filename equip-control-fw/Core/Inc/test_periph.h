#ifndef TEST_PERIPH_H
#define TEST_PERIPH_H

/*
 * test_periph.h — 주변장치 연결 테스트
 *
 * 빌드 플래그 -DTEST_MODE 를 추가하면 FreeRTOS 대신 테스트 루틴이 실행된다.
 *
 * 테스트 순서:
 *   1. 부저    (PB10, D6)
 *   2. 택트 스위치 (PA10, D2)
 *   3. 포텐셔미터  (PA0,  A0  — ADC1 CH0)
 *   4. INA219  (PB8 SCL / PB9 SDA — I2C1)
 *   5. 팬+IRF520 (PC7,  D9)
 */

#ifdef TEST_MODE
void test_periph_run(void);
#endif

#endif /* TEST_PERIPH_H */
