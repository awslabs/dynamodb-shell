// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ddbsh.cpp
 *
 * The main class for ddbsh, instantiated from main.cpp
 *
 * Amrith. 2022-08-10
 *
 */

#include "ddbsh.hpp"
#include "command.hpp"
#include <parser-defs.hpp>
#include <string>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdlib.h>
#include "logging.hpp"
#include <stdio.h>
#include "allocation.hpp"

#include <iostream>

#include "aws-includes.hpp"

using namespace ddbsh;
#include "parser.hpp"
#include "ddbsh_ddb_client_config.hpp"

extern int yylineno;
extern "C" int yylex(void);
extern int yyparse(CCommand **);
extern "C" FILE* yyin;

CDDBSh::CDDBSh()
{
    m_quit = false;

    if (std::getenv("DDBSH_CONFIG_FILE") != nullptr)
        m_config_file = std::getenv("DDBSH_CONFIG_FILE");
    else
    {
        m_config_file = getenv("HOME");
        m_config_file += "/.ddbsh_config";
    }

    // the preferred environment variable is AWS_REGION, which
    // deprecates AWS_DEFAULT_REGION. If nothing is provided, use the
    // default of us-east-1
    if (std::getenv("AWS_REGION") != nullptr)
	m_region = std::getenv("AWS_REGION");
    else if (std::getenv("AWS_DEFAULT_REGION") != nullptr)
        m_region = std::getenv("AWS_DEFAULT_REGION");
    else
        m_region = "us-east-1";

    if (std::getenv("DDBSH_ENDPOINT_OVERRIDE") != nullptr)
        m_endpoint = std::getenv("DDBSH_ENDPOINT_OVERRIDE");
    else
        m_endpoint = "";

    // load history file
    m_history_file = getenv("HOME");
    m_history_file += "/.ddbsh_history";
    read_history(m_history_file.c_str());

    p_dynamoDBClient = NULL;
    m_reconnect = true;

    Aws::InitAPI(m_options);

    // initializer
    st_uniq = 0;
}

CDDBSh::~CDDBSh()
{
    write_history(m_history_file.c_str());

    delete p_dynamoDBClient;
    Aws::ShutdownAPI(m_options);
}

CDDBSh * CDDBSh::getInstance()
{
    static CDDBSh me;
    return &me;
}

void CDDBSh::quit()
{
    m_quit = true;
}

int CDDBSh::run()
{
    CCommand * pCommand = NULL;
    int retval = 0;

    // this is used only in readline anyway.
    yyin = stdin;
    m_quit = false;

    do
    {
        int allocs, frees;

        yylineno = 1;

        logdebug("[%s, %d] At the top of run().\n", __FILENAME__, __LINE__);

        if (m_reconnect)
            reconnect();

        yyparse(&pCommand);

        logdebug("[%s, %d] Done with yyparse(). pCommand = %p\n",
                 __FILENAME__, __LINE__, pCommand);

        if (pCommand)
        {
            logdebug("[%s, %d] Will invoke run().\n", __FILENAME__, __LINE__);
            retval = pCommand->run();
            logdebug("[%s, %d] Done with run() retval = %d.\n", __FILENAME__, __LINE__, retval);
            delete pCommand;
            pCommand = NULL;
        }

#ifdef TEST_HARNESS
        get_allocations(&allocs, &frees);
        if (allocs != frees)
        {
            logerror("[%s, %d] memory leak - allocations = %d, frees = %d\n",
                     __FILENAME__, __LINE__, allocs, frees);
            show_allocations();
            exit(1);
        }
        else
        {
            logdebug("[%s, %d] no memory leak - allocations = %d, frees = %d\n",
                     __FILENAME__, __LINE__, allocs, frees);
        }
#endif
    } while (!feof(yyin) && !m_quit && (!retval || !abort_on_error()));

    return retval;
}

void CDDBSh::setRegionAndEndpoint(std::string region, std::string endpoint)
{
    m_region = region;
    m_endpoint = endpoint;

    m_reconnect = true;
}

std::string CDDBSh::getRegion()
{
    if (m_endpoint.empty())
        return m_region;
    else
        return m_region + " (*)";
}

// In order to handle delegated access and the AWS_USER_PROFILE get
// the credentials to use. The method here is based on
// https://github.com/aws/aws-sdk-cpp/issues/150#issuecomment-416646025

// get the value of the AWS User Profile (if one is set) or "default",
// the default. AWS_PROFILE is the (now) preferred environment
// variable and it deprecates AWS_DEFAULT_PROFILE.
std::string CDDBSh::getProfileName()
{
    std::string profile;
    if (std::getenv("AWS_PROFILE") != nullptr)
	profile = std::getenv("AWS_PROFILE");
    else if (std::getenv("AWS_DEFAULT_PROFILE") != nullptr)
	profile = std::getenv("AWS_DEFAULT_PROFILE");
    else
	profile = "default";

    logdebug("[%s, %d] Using profile %s\n", __FILENAME__, __LINE__, profile.c_str());
    return profile;
}

// get the current configuration profile that should be used.
Aws::Config::Profile CDDBSh::getProfile()
{
    std::string profile_name = getProfileName();

    // credentials file does not prefix profile names with the word
    // "profile" therefore the loader is provided a second parameter
    // of false (which is the default).
    Aws::Config::AWSConfigFileProfileConfigLoader credLoader(
	Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename(), false);

    if (credLoader.Load())
    {
	logdebug("[%s, %d] Looking for named credential profile. %s\n",
		 __FILENAME__, __LINE__, profile_name.c_str());
	// look for the named profile
	auto credIter = credLoader.GetProfiles().find(profile_name);
	if (credIter != credLoader.GetProfiles().end())
	{
	    logdebug("[%s, %d] Found named credential profile. %s\n",
		 __FILENAME__, __LINE__, profile_name.c_str());

	    return credIter->second;
	}
    }

    // either the loader failed to load a file, or the profile (by
    // name) wasn't found. Note that the config file does prefix
    // profile names with the word "profile". Therefore specify the
    // second parameter (true), default is false.

    Aws::Config::AWSConfigFileProfileConfigLoader configLoader(Aws::Auth::GetConfigProfileFilename(), true);
    if (configLoader.Load())
    {
	logdebug("[%s, %d] Looking for named config profile. %s\n",
		 __FILENAME__, __LINE__, profile_name.c_str());

	// look for the named profile
	auto configIter = configLoader.GetProfiles().find(profile_name);
	if (configIter != configLoader.GetProfiles().end())
	{
	    logdebug("[%s, %d] Found named config profile. %s\n",
		 __FILENAME__, __LINE__, profile_name.c_str());

	    return configIter->second;
	}
    }

    // Let's see whether we find a malformed config file which doesn't
    // have the "profile" prefix, and generate a useful error for the
    // user.
    Aws::Config::AWSConfigFileProfileConfigLoader badConfigLoader(Aws::Auth::GetConfigProfileFilename(), false);
    if (badConfigLoader.Load())
    {
	logdebug("[%s, %d] Looking for a malformed named config profile. %s\n",
		 __FILENAME__, __LINE__, profile_name.c_str());

	auto configIter = badConfigLoader.GetProfiles().find(profile_name);
	if (configIter != badConfigLoader.GetProfiles().end())
	{
	    logerror("[%s, %d] Possibly malformed config profile (%s) found. "
		     "Profiles in the config file must have the prefix 'profile'.\n",
		     __FILENAME__, __LINE__, profile_name.c_str());
	}
    }

    // no named profile could be found.
    logdebug("[%s, %d] Using default (empty) profile.\n", __FILENAME__, __LINE__);
    return Aws::Config::Profile();
}

Aws::Auth::AWSCredentials CDDBSh::getCredentials()
{
    // Does the default credentials provider chain help?
    Aws::Auth::DefaultAWSCredentialsProviderChain defaultProviderChain;
    Aws::Auth::AWSCredentials credentials = defaultProviderChain.GetAWSCredentials();
    if(!credentials.GetAWSAccessKeyId().empty() && !credentials.GetAWSSecretKey().empty())
    {
	logdebug("[%s, %d] Using Access Key from the default provider chain. %s\n",
		 __FILENAME__, __LINE__, credentials.GetAWSAccessKeyId().c_str());
	return credentials;
    }

    //  default provider chain didn't work, look for current profile.
    Aws::Config::Profile profile = getProfile();
    Aws::String roleArn = profile.GetRoleArn();
    Aws::String sourceProfile = profile.GetSourceProfile();

    if(roleArn.empty() || sourceProfile.empty())
    {
	// no match, use the empty credentials
	logdebug("[%s, %d] no profile credentials found, using default.\n",
		 __FILENAME__, __LINE__);
	return Aws::Auth::AWSCredentials("", "");
    }

    // We got role assumption here.
    Aws::Auth::ProfileConfigFileAWSCredentialsProvider provider(sourceProfile.c_str());
    Aws::STS::Model::AssumeRoleRequest roleRequest;

    roleRequest.SetRoleArn(roleArn);
    roleRequest.SetRoleSessionName(provider.GetAWSCredentials().GetAWSAccessKeyId());

    Aws::STS::STSClient stsClient(provider.GetAWSCredentials());
    Aws::STS::Model::AssumeRoleOutcome response = stsClient.AssumeRole(roleRequest);

    if(!response.IsSuccess())
    {
	logdebug("[%s, %d] Failed to assume role %s. Using default.\n", __FILENAME__, __LINE__,
		 roleArn.c_str());
        return Aws::Auth::AWSCredentials("", "");
    }

    logdebug("[%s, %d] Assumed role %s\n", __FILENAME__, __LINE__, roleArn.c_str());
    Aws::STS::Model::Credentials roleCredentials = response.GetResult().GetCredentials();
    return Aws::Auth::AWSCredentials(roleCredentials.GetAccessKeyId(),
                                     roleCredentials.GetSecretAccessKey(),
                                     roleCredentials.GetSessionToken());
}

void CDDBSh::reconnect()
{
    logdebug("[%s, %d] Will attempt reconnect.\n", __FILENAME__, __LINE__);
    delete p_dynamoDBClient;

    CDDBShDDBClientConfig clientConfig(m_region, m_endpoint);
    p_dynamoDBClient = new Aws::DynamoDB::DynamoDBClient(
	getCredentials(), clientConfig);

    if(checkConnection())
        m_reconnect = false;
    else
        logdebug("[%s, %d] The reconnect was successful.\n",
                 __FILENAME__, __LINE__);

    return;
}

bool CDDBSh::checkConnection()
{
    Aws::DynamoDB::Model::ListTablesRequest listTablesRequest;
    listTablesRequest.SetLimit(2);

    const Aws::DynamoDB::Model::ListTablesOutcome& lto = p_dynamoDBClient->ListTables(listTablesRequest);
    if (!lto.IsSuccess())
    {
        logerror("[%s, %d], Connection Failed. %s. %s. \n\t%s\n",
                 __FILENAME__, __LINE__,
                 lto.GetError().GetExceptionName().c_str(),
                 lto.GetError().GetRequestId().c_str(),
                 lto.GetError().GetMessage().c_str());
        return false;
    }

    return true;
}

Aws::DynamoDB::DynamoDBClient * CDDBSh::getClient()
{
    return p_dynamoDBClient;
}

#define DDBSH_UNIQUE_PREFIX_LEN   (3)
#define DDBSH_UNIQUE_PREFIX_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
std::string CDDBSh::st_unique_prefix()
{
    int val = this->st_uniq++, b;
    std::string prefixchars = DDBSH_UNIQUE_PREFIX_CHARS;
    std::string prefix;
    for (int ix = 0; ix < DDBSH_UNIQUE_PREFIX_LEN; ix ++)
    {
        prefix += prefixchars[val % prefixchars.length()];
        val /= prefixchars.length();
    }

    return std::string(prefix);
}
