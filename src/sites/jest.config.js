module.exports = {
    testEnvironment: "node",
    moduleFileExtensions: ["ts", "js"],
    transform: {
        "^.+\\.tsx?$": ["ts-jest", {
            tsconfig: {
                types: ["jest", "node"],
            },
            diagnostics: {
                ignoreCodes: [151001],
            },
        }],
    },

    // Code coverage
    collectCoverage: true,
    coverageDirectory: "../../coverage/",
    collectCoverageFrom: [
        "**/*.ts",
        "!**/node_modules/**",
        "!test-*.ts",
    ],
};