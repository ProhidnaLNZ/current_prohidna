inline String generateUUID() {
    uint64_t rnd = ((uint64_t)esp_random() << 32) | esp_random();
    char uuid[17];
    snprintf(uuid, sizeof(uuid), "%016llx", rnd);
    return String(uuid);
}