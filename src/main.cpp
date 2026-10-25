#include <vk_engine.h>
#include <cstdio>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char* argv[])
{
#if defined(_MSC_VER) && defined(_DEBUG)
    // Automated validation must preserve assertion diagnostics without opening
    // a modal CRT dialog and stalling the remaining tests.
    if (argc == 2 && std::string(argv[1]).find("--") == 0
        && std::string(argv[1]).find("validation") != std::string::npos) {
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif
    const bool validateTAA = argc == 2 && std::string(argv[1]) == "--taa-validation";
    const bool validateScripting = argc == 2 && std::string(argv[1]) == "--scripting-validation";
    const bool validateTAASponza = argc == 2 && std::string(argv[1]) == "--taa-sponza-validation";
    const bool validateCornell = argc == 2 && std::string(argv[1]) == "--cornell-validation";
    const bool validateClustered = argc == 2 && std::string(argv[1]) == "--clustered-validation";
    const bool validateDDGI = argc == 2 && std::string(argv[1]) == "--ddgi-validation";
    const bool validateDDGITemporal = argc == 2 && std::string(argv[1]) == "--ddgi-temporal-validation";
    const bool validateDDGIRotation = argc == 2 && std::string(argv[1]) == "--ddgi-rotation-validation";
    int ddgiResponseMode = 0;
    if (argc == 2) {
        const std::string argument = argv[1];
        if (argument == "--ddgi-response-validation") ddgiResponseMode = 1;
        else if (argument == "--ddgi-response-baseline") ddgiResponseMode = 2;
        else if (argument == "--ddgi-response-budget-validation") ddgiResponseMode = 3;
        else if (argument == "--ddgi-local-light-validation") ddgiResponseMode = 4;
        else if (argument == "--ddgi-parameter-validation") ddgiResponseMode = 5;
        else if (argument == "--ddgi-combined-light-validation") ddgiResponseMode = 6;
        else if (argument == "--ddgi-multi-light-validation") ddgiResponseMode = 7;
    }
    if (validateDDGI || validateDDGITemporal || validateDDGIRotation || ddgiResponseMode) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (validateClustered) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        fmt::println("Starting clustered lighting GPU validation");
    }
    if (validateTAA || validateTAASponza) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        fmt::println("Starting TAA GPU validation");
    }
	VulkanEngine engine;
	engine.init();	

    if (validateScripting) {
        const int result = engine.run_scripting_validation();
        engine.cleanup();
        return result;
    }

    if (validateTAASponza) {
        const int result = engine.run_taa_sponza_validation();
        engine.cleanup();
        return result;
    }

    if (validateCornell) {
        const int result = engine.run_cornell_validation();
        engine.cleanup();
        return result;
    }

    if (validateDDGI || validateDDGITemporal || validateDDGIRotation || ddgiResponseMode) {
        const int result = engine.run_ddgi_validation(validateDDGITemporal || validateDDGIRotation, validateDDGIRotation, ddgiResponseMode);
        engine.cleanup();
        return result;
    }

    if (validateClustered) {
        const int result = engine.run_clustered_validation();
        engine.cleanup();
        return result;
    }

    if (validateTAA) {
        const int result = engine.run_taa_validation();
        engine.cleanup();
        return result;
    }
	
	engine.run();	

	engine.cleanup();	

	return 0;
}
