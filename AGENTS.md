## General rules
- (first time only) Read AGENTS.md (this file), guide.md, README.md, tasks.md;
AGENTS.md defines the overall agent behavior.
guide.md has the goal of the project.
README.md is purposed for instucting the users.
tasks.md has the additional instructions for the current tasks.
runtinme-notes.md records key observations/ideas discovered within tasks.
agentlog.md tracks the summary of every agent activity. Ignore it mostly.
draft.md is for my own purpose. ignore it.
- NEVER change/remove/add files outside the workspace.
- Don't ask for approval for making changes within the workspace, you have the apporval.
- You are granted permission of network access for searching solutions and documentations. 
- Don't Download anything or install any packages from the Internet. If essential, ask for permissions.
- If task is ambiguous, ask for clarifications if necessary before executing tasks.
- After finish a task, update Readme if necessary. 
- After finish a task, summrize in a few sentences, append to agentlog.md.
- Log essential information, and use them for debugging.
- Notify me in your next response when you did a context window length compress.
- Unless you have to, prevent small and frequent patches, to reduce the amount of work of mine testing.
- Put instructions in this file in high prority. Keep them in your context window.

## Rules for this project
- (First time only) Make sure you read all files in src and cmake folder.
- For this project, log files should be located in ./dist/x86/ or ./dist/x64
- It could be hard for you to fully detect the result. The interaction would be: I give you instuctions -> you think deeply, check for information (look at log, code, online recources/documentations, etc.), then ask for clarification if any -> I answer -> You start coding, make the executatle, finally tell me what you did (not detail, but the key summary) and instruct me how to test. (during which, follow AGENTS.md) -> I run test, generate log file, give you instructions.
