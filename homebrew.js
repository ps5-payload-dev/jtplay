async function main() {
    const CWD = window.workingDir;

    return {
        mainText: "JTPlay",
	secondaryText: 'A mediaplayer',
	imgPath: baseURL + "/fs/" + CWD + '/assets/icons/logo.png',
	onclick: async () => {
	    return {
		path: CWD + '/jtplay',
		cwd: CWD,
		args: ['--assets', CWD + '/assets',
		       '--fonts', CWD + '/fonts',
		       '--plugins', CWD + '/plugins',
		       '--cache', CWD + '/cache'],
		env: {CURL_CA_BUNDLE: CWD + '/ca-bundle.crt'}
	    };
        }
    };
}
